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

> **M7 REPLANNED 2026-07-26 after two in-headset runs.** Actor pinning (below) is retired:
> it works mechanically but cannot reach the goal. The rebuild drives the viewmodel at the
> DRAW and BONE level - see the M7-v2 block under the ticked items, and the ARCHITECTURE
> decision log entry of the same date for why.

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
      *UPGRADED 2026-07-28 (session 21): TRUE per-weapon profiles shipped - the R-hand aim
      trim + ray-origin offsets hot-swap keyed by the weapon's CLASS NAME (UObject +0x28
      name / +0x30 class resolved live via GNames), persisted to weapons.ini, saved by the
      preset button, weapon resolved pre-fire via the Base==AHands structural scan. The
      live-switch swap proof is on the session-21 in-headset checklist.*
### M7-v2 - the rebuild (planned 2026-07-26 with the user)

Goal in the user's words: the weapon and the plasmid hand each move as ONE with their own
controller, like a native VR game. Explicitly NOT wanted: bent arms, elbows, IK, dual-wield.
"I don't need it to be perfect - I just want it in sync with the controller."

- [x] ~~Step 1 - find the viewmodel's draw calls~~ / ~~Step 2 - prove render-side control~~
      *SUPERSEDED 2026-07-26 (session 12, user's call: goal-first, any stable method): the
      bone route landed DIRECTLY, so the render-side rungs were never needed. Kept in
      reserve: the fully-designed vm_draw fallback (Map/Unmap CB patching) in the session-12
      plan file, and the enlarged `dumpframe full` capture (256 -> 1344 B, shipped) that
      answers the GPU-vs-CPU-skinning question from one dump if it ever matters.*
- [x] **Step 3 - reach the bone matrices.** *DONE OFFLINE + LIVE 2026-07-26 (session 12,
      ENGINE_NOTES "Skeleton / bone internals"): per-actor `SkeletonInstance` at actor
      +0x3FC, component-space hkQsTransform array (+0x48, 48 B stride, 47 bones on the
      AHands rig), evaluate-if-dirty flag +0x88, freeze +0x20. Live pokes proved per-bone
      writes render the same frame AND the equipped weapon renders from the attach bone's
      entry (bone 43) - engine-side, undistorted.*
- [x] **Step 4 - drive the hands from the bones.** *BUILT + FLAT-VERIFIED 2026-07-26
      (session 12, `bones.cpp`, `vrhands mode bones` = new default): the hand CLUSTER
      (wrist+fingers+attach bone) moves rigidly to the controller pose; simpose series
      rotates about the GRIP on all three axes incl. wrist roll (the actor-pinning lever
      is gone); fire test passed with the drive live (subs, ammo, off-crosshair decal,
      dumps 8->8); sleeve bones collapse to zero scale to hide the arm (default on).
      IN-HEADSET CHECK PENDING.*
- [x] **THE EARLY RISK TEST: do the plasmid's hand FX follow?** *YES - for ENGINE-side
      (bone) writes, proven 2026-07-26 by A/B: with the drive on, Electro Bolt's idle
      electricity wrapped the DRIVEN hand at the synthetic-controller spot; drive off, it
      snapped back to the engine pose. A live cast fired through the anim-notify chain
      with the drive running (ability InitiateDamage called, EVE consumed, wall scorch).
      The render-side path was never used, so its FX limitation never applied.*
- [x] **The camera-coupled rig term (the "follows my head" defect), root-caused and
      countered.** *2026-07-26 session 13: the renderer draws the rig in a FOREGROUND scene
      (fixed 60-deg 4:3 projection + view eye parked ~32 UU behind the rig in ACTOR space +
      hand sway + rigid-section rebake from our own driven bones). bones.cpp "render lock"
      inverts an analytic model of it per frame at gain 0.5; flat simhead sweeps hold the
      anchor within 2-4 deg of world-true through +-30 yaw / +-20 pitch (was 15-25 deg of
      coupling). ENGINE_NOTES "Foreground scene FOV" has the whole chain.*
- [x] **The depth geometry (the "glued to my face / HUD" defect): fixed and verified
      numerically, flat, in stereo.** *2026-07-27 session 14: the fg eye rides the CAMERA
      (translation included, dump-proven) with a true pull-back of 13 UU (calibrated from
      three agreeing physical baselines - the dump-recovered eye is section-frame-relative
      and unusable absolutely); the render lock now solves the anchor at w* = k*trueDistance,
      making apparent size, stereo depth, and translation parallax world-correct at once.
      Acceptance: camera-offset parallax 420 -> 355 px (world-correct 341); size on
      hand-distance doubling 0.605 -> 0.465-0.470 (correct 0.465); depth band clean to
      wSolve ~142; simhead sweep unregressed; fire test clean. Session 13's "rebake
      doubling" decomposed as model-scale error 1.63 x true rebake 1.1; per-axis gains
      (lockgain/lockdgain) default 0.9. ENGINE_NOTES "Foreground scene FOV" session-14
      block has the calibration chain.*
- [x] **The honest lens (the telephoto-composition ceiling): matched, calibrated, shipped
      ON.** *2026-07-27 sessions 15-16: the fg pass's FOV is a live PlayerController field
      (+0x460) consumed per frame; `vrfgfov` writes the world-equivalent spec and the whole
      rig re-lenses to the WORLD lens (dump-proven across all cb tiers). The driven path's
      eye pull at the matched lens calibrated +11.5 UU (NOT fov-coupled - the vanilla
      path's 65 never applies to the driven rig), knob default 12.8 through the 0.9 depth
      gain; the pull rides the ACTOR frame under head-split (simhead-proven, the qd-frame
      overshoot fixed). Full session-14 acceptance ladder re-passed at k=1 on a clean boot
      with shipping defaults: parallax 0.98x, size 1.02-1.05x, sweep glued 2-17 px, fire
      clean, dumps stable. ENGINE_NOTES "Foreground scene FOV" sessions 15-16.*
- [x] **Done when:** the weapon is one with the right controller and the plasmid hand is one
      with the left, each inspectable from any angle, at a believable size, with their effects
      attached. (Arms hidden is a valid and expected answer.) ***M7-v2 IS DONE - the user's
      call 2026-07-27 (session 16 part 4).*** *In-headset verdict (part 2): "fully working,
      and it's not moving with the head/headset/camera anymore" - tracking and head
      decoupling confirmed. SIZE resolved in part 3 by the user's own worldScale-100
      calibration (viewmodel size AND distance both read right - the engine-side mesh-scale
      levers stayed dead and turned out to be unnecessary, ENGINE_NOTES session 16 part 2).
      Effects were proven riding the driven hand in session 12 (Electro Bolt parity + live
      cast). The remaining body-facing coupling is NOT a viewmodel defect - it is a
      camera/locomotion one, split out as M7.5 below.*

- [ ] ~~Done when: hands + current weapon track the controller convincingly~~ (superseded by
      M7-v2 above; wrench melee rides the weapon path for free)
      *Note (user, 2026-07-25): the `AHands` viewmodel is a single mesh (hands + a short
      forearm/sleeve stub), so in M7 the visible arm portion moves RIGIDLY with the weapon/
      controller - good enough for the short FP forearm. Making the ELBOW articulate as the
      hand moves is IK arms (post-v1 below) - the user wants this captured as a post-polish
      focus item.*

## M7.5 - Body/head decoupling follow-up (session 17, ~1 session)

> Split out of M7-v2 on 2026-07-27 (session 16 part 4): the viewmodel milestone is DONE, but
> the user found one root cause that still degrades play - and it is a CAMERA/LOCOMOTION
> defect, not a viewmodel one. Full spec + the invariant in STATUS "Next steps" item 1.

- [x] **Body-follows-head yaw transfer.** SHIPPED session 17, default ON, armed by VR
      PRESET 1; `vrbody off` is the live A/B. The body facing lives at the
      PlayerController's `Rotation.Yaw` (`PC+0x1E8`) - located and proven live: non-zero
      pitch where the pawn's is 0, an additive write holds, the pawn follows for free, the
      engine's own turn composes on our value, and the walk direction follows it to 0.4 deg
      (ENGINE_NOTES session 17). `body.cpp` transfers the head-look yaw once per rendered
      frame behind a probe handshake and a gameplay-view guard.
- [x] **HARD INVARIANT - no regression of head decoupling.** Held exactly, not merely
      within tolerance. The composite `gameYaw - recenterYaw` measured **1.27742 rad at
      every head angle** (0/30/45/90/-45) in both states; at head 45 the hand's world pose
      (`rot.yaw=13323`) and the camera (`camYaw=21516`) were **bit-identical** off vs on
      while gameYaw and recenterYaw each moved +0.78540 rad; `[tlm] yawstep` showed
      **max=0 units, nbig=0** through the arm transient. The yaw path was converted to
      integer rotator units to make the cancellation exact.
- [x] **Feature verified flat.** Walk direction with the transfer OFF: 116.49 and 116.67 deg
      at two head angles 90 deg apart (it tracked the body and ignored the head - the defect
      as a number). With it ON: walk == body == camera to 0.01 deg at both +45 and -45, the
      pair spanning 89.99 deg for a 90 deg head change. Fire test 57->51 for 6 pulls, dumps
      8->8, stereo clean, 20-step +-90 soak returned camYaw and the recenter exactly to
      their starting values.
- [x] **Residual sweep** (partial, and it found something better than expected): with the
      transfer ON the render lock's lateral correction goes **flat within 0.46 UU** across a
      +-30 deg head sweep instead of swinging 10.5 UU, landing on the same 4.57 the
      calibrated zero-split configuration uses - so the transfer restores the
      headset-verified session-16 regime at every head angle. Full table in ENGINE_NOTES.
- [ ] **Still open - the exact cull angle.** The 0/30/60/80/90/100/120 deg simpose sweep and
      the cull-on/off boundary in both directions were NOT measured: NCC template chaining
      broke down across the composition changes (correlations 0.56-0.79, every number void -
      recorded in ENGINE_NOTES so it is not retried blind). Needs a template-free instrument.
      Note the sweep also exposed that the two physical cases differ: a head-only glance with
      the hand parked in the world *increases* hand-vs-body, while the reported case (the
      user swivels, so head and hand rotate together) drives it to ~0.
- [x] **Done when:** the user confirms in the headset. **PASSED 2026-07-27** - "this is
      perfect... the stick was working as expected, the models didn't move when I moved my
      head". One tuning change, now the shipped default: `vrbody deadzone` 0 -> **23 deg**,
      which removes the last "the gun moves with the camera a bit" percept (inside the band
      the body does not steer, so a glance leaves the viewmodel world-locked; beyond it the
      body trails the head by exactly the band width). M7.5 DONE.

## M8 - Release quick phase + HUD usability (~1–2 sessions)

> **Session 28 (2026-07-30) closed both of session 27's open bugs, in-headset accepted.**
> - [x] **Yaw warping on head turn at non-16:9 resolutions (RELEASE BLOCKER)** - the live
>       rendered-FOV watch was reporting the FOREGROUND lens as the world lens (a frame carries
>       two lenses with opposite aspect conventions that coincide only at 16:9), so the mismatch
>       verdict latched during normal gameplay and the projection claim was substituted with the
>       viewmodel frustum - a 1.84x under-claim. The watch now stride-samples across the whole
>       pass and votes. Measured, not inferred: ENGINE_NOTES "Session 28".
> - [x] **Viewmodel/hands moving with the head at non-16:9** - the SAME defect from the other
>       side: one claim cannot serve two different lenses, so fixing the world moved the error
>       onto the hands. The fg match constant is now `(4/3)*(h/w)` instead of a hardcoded `0.75`
>       (which is that expression at 16:9), and `bones.cpp`'s world and fg lens models read the
>       live aspect. Accepted in-headset with NO re-tune of the session-16 offsets.
> - [x] **VR freezes permanently after alt-tab (RELEASE BLOCKER)** - a circular wait: the M8
>       pace guard submitted nothing while unfocused, and VDXR will not re-grant FOCUSED to an
>       app that submits nothing. `xrWaitFrame` moved to a dedicated pace thread with a deadline
>       on the present thread, which also retires the session-26 hang class instead of trading it
>       for this freeze. Third attempt at this bug class, first one built on a measurement.
> - [x] Lens math verified DYNAMIC across resolutions (k=1.333333 at 2048x2048, exactly
>       0.750000 at 1920x1080), so 16:9 is bit-identical to prior releases by construction.

> **Restructured 2026-07-27 (session 16 part 4, user's call):** the selection wheels moved
> to post-v1 (the current switching UI is good enough); in their place, a QUICK RELEASE
> phase right after session 17, then HUD usability.

**Quick phase (immediately after session 17). The two defects below are RELEASE BLOCKERS
by the user's call 2026-07-27 - they hit every user, not just this desk, so they ship
fixed or the release waits:**
- [x] **Flat-screen mirror under stereo (RELEASE BLOCKER)** - DONE flat 2026-07-27
      (session 18): the window is pinned to the LEFT eye (left presents snapshot the
      backbuffer, right presents re-blit it after the right eye's XR capture; also active
      with no open XR frame). `vrmirror off` = the old alternation. Flat acceptance:
      within-condition shot diffs at the 0.3-0.7 floor, mirror-on vs mirror-off cross-diff
      13.6-13.9 (the near-field eye offset) - the pin flips the displayed eye; holds ==
      blits exactly. Headset-side confirmation (stereo still correct in the HMD) is on the
      session-18 checklist. ENGINE_NOTES session 18 explains why single screenshots always
      looked same-phase (present duty cycle, not absence of alternation).
- [x] **Headset-disconnect stall (RELEASE BLOCKER)** - DONE flat 2026-07-27 (session 18):
      the pace guard skips per-present xrWaitFrame once a previously-FOCUSED session
      leaves FOCUSED (bring-up exempt; 5 s keepalive as recovery insurance; `vrpace off`
      = old behavior). Flat, with a simulated idle (`vrpace simidle on`, 1 s block per
      paced frame): guard ON holds 378-396 presents/s, guard OFF collapses to 1/s, guard
      re-on recovers to 416/s within seconds. Real-headset idle/reconnect is on the
      session-18 checklist (the >1 s wait logging will show how VDXR behaves).
- [ ] **First GitHub release**: README rewritten with install (proxy DLLs, itsloopyo
      conflict, VDXR setup) and VR usage (PRESET 1, sliders, vrpreset save, per-hand
      offsets); release zip built (both RelWithDebInfo DLLs + README.txt). REMAINING:
      the user's in-headset verification, then tag + publish (user gate - nothing public
      without their go).
- [x] **Hand-switch wrong-controller bug (user report 2026-07-27)** - root-caused and
      FIXED flat 2026-07-27 (session 18): grips compose to the bumpers, and the auto-hand
      latch learned only from triggers, so a grip switch left the model+laser on the stale
      controller until the next pull (hence the predicted self-correction). The latch now
      learns from the composed bumpers too (LB -> left/plasmid, RB -> right/weapon;
      triggers still win same-frame). Flat: status auto(R) -> LB alone -> auto(L) -> RB
      alone -> auto(R), no trigger events. Headset confirmation on the checklist.

- [x] **Per-hand model offsets (user ask 2026-07-27, session 18)**: the viewmodel's
      position/rotation offsets are per hand (L plasmid / R weapon) - `vrhands pos|rot
      [l|r] ...` (no side = both, harness-compatible), a "Tuning hand: L / R" selector
      over the six sliders, per-hand hands.ini keys with legacy fallback, and `vrpreset
      save` now persists them too. Flat-proven: each hand's offsets apply only while that
      hand drives (0.01 UU exact), ini round-trips both formats.
- [x] **Aim-ray origin offsets (user ask 2026-07-27, session 18 part 2)**: per-hand
      `vraim pos [l|r] <fwd> <right> <up>` cm + "Ray offset hand" selector sliders,
      applied once at ray build so laser + bullets + substitution move as one while the
      tuned model stays put; vrpreset.ini persistence. Flat-exact (+-60/45 UU, per-hand
      isolated, subs carried, round-trip).
- [x] **Crosshair hidden by default (user ask 2026-07-27, session 18 part 2)**: the
      engine-native lever `ShockPlayer.bReticleDisabled` written via the engine console
      SET handler through the exec seam (the reusable name-based property-write find);
      `vrxhair on|off` + overlay checkbox, 15 s re-assert, persisted. Flat: 19 -> 0
      bright center pixels on a clean boot, toggle exact both ways.

**HUD usability:**
- [x] See health + EVE clearly in VR - DONE flat 2026-07-28 (session 19): the gameswf
      HUD draws are classified per present interval (scene-vote + tonemap detection,
      corrected fingerprint in ENGINE_NOTES) and redirected to an offscreen RT; the
      alpha-repaired copy feeds a head-locked XrCompositionLayerQuad (distance/width/
      height sliders, vrpreset.ini) AND a post-capture window composite, so the flat
      window keeps its HUD while both eyes come out clean - this also fixes the old
      "HUD in both eyes" defect, and the pause menu lands on the readable quad for
      free. Flat: 119 HUD draws/interval classified, leaks=0, redirect removes the
      HUD from the frame, composite restores the window, round-trips exact. The
      in-headset quad verdict is on the session-19 checklist.
- [x] Keybind audit on Quest 3 Touch - DONE flat 2026-07-28 (session 19): ground truth
      pulled from User.ini XENON_* (ENGINE_NOTES - the game layout is A=Use, B=Heal,
      X=Reload/Hack/EVE, Y=Jump; DPAD_UP/DOWN cycle ammo, flat-proven); the XR layer
      re-routes Touch A->jump, B->use, Y->heal, X->reload, and right-stick Y FLICKS
      (freed by the stick-pitch kill) pulse dpad up/down = ammo-type cycling. Headset
      walkthrough on the checklist.
- [x] **Done when:** a friend can install from the release zip, press VR PRESET 1, see
      their health/EVE, and every binding they need works - and someone watching the
      monitor sees a normal single-eye picture, including after the headset comes off.
      **M8 COMPLETE 2026-07-28: v0.2.0 published**
      (https://github.com/mohamad-balouza/bioshock-vr/releases/tag/v0.2.0) after two
      in-headset passes ("looks amazing") + the feedback round (stencil masks, true
      alpha coverage, A/B swap, click-held ammo select, wheel pitch guard, zoom
      removed) - every fix flat-verified then headset-confirmed.

## M9 - Comfort + UI/config + release polish (~2–3 sessions)

**Two-hand track (user's call 2026-07-28, sequenced AFTER the session-20 aim work -
which SHIPPED 2026-07-28 on branch s20-aim-sync: one trim algebra (28.21 -> 0.03 deg),
vrrec record+replay, FName/GNames + named skeleton dumps, the muzzle ray, the idle-sway
kill. Both prerequisites below are now MET.):**
- [ ] **Off-hand tracking** (`vrhands offhand track|hide`, default hide until judged by
      eye): drive the INACTIVE hand's cluster from its controller instead of collapsing
      it - the same rigid drive run on both clusters per frame. Reload/idle anims on the
      tracked hand are overwritten by construction (the weapon's own skeleton still
      animates itself). The open question is the off-hand's SHAPE (the engine's last
      evaluated pose may be a grab/reach); mitigation on file: snapshot a good cluster
      shape (e.g. the plasmid idle) and use it as the off-hand's fixed reference.
      Prerequisite: none hard, but do it after the session-20 trim unification so both
      hands ride one algebra.
- [ ] **Two-handed weapon handling** (shotgun/MG/GL/crossbow/chemical thrower): left
      hand within ~10 cm of the foregrip + grip = engage; weapon then aims along the
      rear-hand -> front-hand line (a different ray source into the same fire-seam
      substitution; the model gets the same target rotation) and the left cluster sits
      at "bone-43 world transform + per-weapon foregrip offset" - a transform we WRITE,
      so no engine cooperation needed. Release on grip-off or distance. Purely
      presentation + aim math - the game has no two-hand mechanic to desync. What makes
      it cheap here: no arms/elbows = no IK, and the art already contains authored
      grip shapes to snapshot (the shotgun's two-hand idle). PREREQUISITES, in order:
      session-20 trim-algebra unification (do not build two-hand aim on the algebra
      being replaced), then FName/per-weapon identity + the muzzle/foregrip bone probe
      (per-weapon foregrip offsets; the weapon skeletons may carry foregrip bones).
      Per-weapon engage radius + offsets tuned by eye.

- [x] **THE WRENCH SOMETIMES DOES NOT HIT** - DONE session 30 (2026-07-30), accepted
      in-headset: *"it's working and I was able to hit him consistently."* **The engine's own
      view pitch was frozen at -88.9 degrees, straight down.** Pitch kill zeroes the composed
      right-stick Y so the stick cannot fight the HMD - but zeroing an INPUT does not set a
      value, so the engine's pitch could never change again; and the camera write is
      asymmetric, yaw RELATIVE (engine's own plus a head residual, so it stays real) and
      pitch ABSOLUTE from the head, discarding the engine's value unread. Nothing corrects
      it and nothing notices, because the rendered view is the head's either way. Melee aims
      with that number, which is why walls connected (approached level), fights missed into
      the floor, the opening rocks miss with no combat at all (rocks are on the floor), and
      guns were fine (we substitute the whole fire ray at a seam melee lacks).
      Fixed by SERVOING instead of zeroing: the game layer publishes head-pitch minus
      engine-pitch each CalcView and the bridge feeds a proportional stick value, so the game
      steers its own pitch through its own input path and no engine memory is written.
      `vrinput pitchservo on|off|invert|status`. Two hypotheses died first, both by
      measurement: the aim seams (melee reaches neither) and soft lock-on (radius 5000 feels
      identical to 0, so that write never reaches the live object).
      *Residual, measured: converges to within 4-8 deg rather than 0, because near
      convergence the proportional stick falls under the GAME's own deadzone. Inside melee
      tolerance; closing it needs a minimum stick magnitude and risks a limit cycle.*
- [x] **Wrench swing gesture (user's call 2026-07-28)** - DONE session 31 (2026-07-31),
      **accepted in-headset: "I tested it and it's perfect."** A fast right-hand motion composes a
      full RT pulse while the wrench is equipped, IN ADDITION to the trigger (user's call -
      the trigger keeps working unchanged, so there is no regression to roll back).
      `core/input/swing.{h,cpp}`, gate published each CalcView as
      `strictGameplay && aim::weapon_key_is("Wrench")`, velocity by finite difference of
      the hand-pose funnel (head-relative by default), fire on the rising edge of the
      speed threshold, hysteresis re-arm + cooldown against double-fires.
      `vrinput swing on|off|status|threshold|rearm|cooldown|pulse|delay|rel|log|sim`,
      persisted in vrpreset.ini, overlay checkbox + sliders.
      *Flat-measured: a 120 ms RT pulse fires the weapon (two pulses = two shots), and every
      `[swing] FIRE` was followed 8-11 ms later by the engine's own weapon-fire seam at
      rt=255. Gate closed with a gun equipped, cooldown, re-arm hysteresis, wheel
      suppression, off-switch and the ini round-trip all pass.*
      *Shipped defaults after the headset run: **ON**, fire threshold **3.6 m/s** (the user's
      own call, replacing the 2.2 guess that shipped to that run - high enough that walking,
      turning your body or reaching for something never registers), re-arm 1.0 m/s, cooldown
      300 ms, pulse 120 ms, delay 0.* No further tuning was asked for; `swing delay <ms>`
      remains if the contact point ever wants moving.
- [x] **Kill the first-boot restart** - DONE session 22 (2026-07-29): compose_over answers
      a failed slot-0 GetState with a neutral connected pad while vrinput is off. Virgin
      gate PASSED: menu prompts stayed KB/M with the phantom pad idle; `vrinput on`
      mid-session put the IAT lane at ~680 polls/s with no restart. Marker file + README
      note retired.
- [x] Snap turn + smooth-turn speed slider - DONE session 22 (recenter-composite steps
      carried by the body transfer, +-8192 units exact per 45-deg pulse; turnScale on the
      composed stick; both persisted + overlay). Height/seated recenter, optional vignette
      still open:
- [ ] Height/seated recenter, optional vignette
- [x] **Cinematics in VR** - DONE session 22 (2026-07-29), and BOTH on-file hypotheses were
      disproven by measurement: the descent keeps CalcView firing (strict stays GAMEPLAY)
      and the renderer consumes our camera - it renders its OWN 104-deg fov while the
      option/claim says 130; the claim mismatch was the whole fisheye/no-fusion percept
      (ENGINE_NOTES session 22). Shipped: a live rendered-fov watch (per-present cb0
      tangent decode) + the cinematic fallback keyed on strict-false/staleness/
      fov-mismatch/screen-only. DEFAULT = stereo projection with the claim fixed to the
      measured fov (user's call - full stereo cinematics with head-look); the big-screen
      quad is the overlay/vrcine toggle. In-headset verdict pending.
- [x] **Fullscreen flash screens + effects per-kind routing** - DONE session 22: (a) hack +
      loading (and the main menu) are PURE gameswf intervals with the world pass absent
      (dump-proven 0 DrawIndexed) -> the new screen-only detector drops them to the
      readable screen; (b) FMV-class screens ride the same leg; (c) the alcohol-blur
      composite (post-tonemap draw sampling a backbuffer-sized texture) now stays
      IN-FRAME per eye, never on the panel (postFx=0 false positives in gameplay).
      In-headset verdict pending (incl. the Big Daddy FMV, no flat repro exists).
      *Session 30 correction: (c)'s size test is DEGENERATE at a square render
      target and the "postFx=0 false positives" claim only held at 16:9. At
      2048x2048 the backbuffer IS 2048x2048, so the game's own UI atlases matched
      it - measured `postFxRejected=1604161` against `postFx=2` genuine, ~30 HUD
      draws per interval leaking in-frame, 43% of them stranded onto the panel and
      57% into the eye image, i.e. routed by draw order. Now discriminated on bind
      flags (a post-FX source was RENDERED, an atlas never is), which holds at
      every resolution. `vrcine postfx size|rt` keeps the old rule for an A/B.*
- [ ] **Full-screen effects still do not cover the whole view** (user, in-headset,
      after session 29 routed the fill in-frame). Session 30 excluded BOTH routing
      hypotheses and re-scoped it as a GEOMETRY problem. Routing: per-reason
      pass/STRANDED counters read `effect=127010/0` with the redirect armed,
      validated by a one-shot device check and a positive control that made the
      same counter read 36140. And "put it on a different render target" cannot
      work at all - the user's report that the effect is *"the size of the HUD or
      the size of the old resolution"* identifies these as gameswf STAGE-space
      draws, so routing one in-frame makes it stage-sized INSIDE the view rather
      than covering it. Session 29's fix is therefore reverted to the panel by
      default, which also fixed the health/EVE bar colour regression it had caused
      (the bar fills carry the identical fingerprint - see the entry above).
      Remaining fix candidates, in order: patch the dynamic vertex buffer in flight
      (scoped by bbox so the bar fills cannot be caught); our own full-screen quad
      (blocked - the fill colour is in a PS constant buffer and the dump records
      only VS b0..b2); an SWF edit (precedent exists but ships a modified asset).
      Measure first with `img-diff.ps1 -Grid/-Bands`, which is built and
      self-tested, and ask the free question: does the effect stop before the
      picture stops, or do they end at the same edge? If together, the fill is fine
      and the defect is the projection claim - a different fix, unmeasurable flat.
- [x] **HUD health/EVE bars lost their colour** - DONE session 30, a regression
      shipped by session 29's effects change and caught by the user. The bar COLOUR
      fills are textureless 5-vertex gameswf quads, identical to the "effect" fill
      by every test the classifier can apply, so in-frame routing sent them into
      the eye image while their frames stayed on the panel. The counter had been
      saying so all along - `effectsInFrame` advances by exactly 2 per interval,
      every interval, with nothing on screen. Two bars. Default back to panel;
      accepted in-headset.
- [x] **Head-ROLL eye separation bug** - DONE session 22: apply_eye_offset offsets along
      the FULL rotation's right axis (ue_rot_basis) and the AER path shares the one
      implementation. Eye delta measured (6.3,0,0)/(4.455,0,-4.454)/(0,0,-6.3) UU at
      roll 0/45/90 - rotates exactly with the head, bit-identical at roll 0.
- [ ] **Movement wonkiness investigation (user report 2026-07-28, deadzone 0 did not fix)**:
      instrument first, then decide between bodyRate smoothing, a stick response curve, or
      hardware noise. Do not tune blind. *Session 22 shipped the instrument* - `vrinput
      sticklog on` logs the FINAL composed pad at 10 Hz (post merge/pitchkill/turn) and
      last_composed_sticks() exists for the recorder; pair with `vrbody probe on` resid
      lines on a real headset walk. The capture + analysis are still open.
- [x] **Cinematic letterbox BARS** - DONE session 29 (2026-07-30), and the mechanism was
      the problem all along: the bars are a gameswf DRAW (`WidescreenBars`, character 292
      in HUDPC.swf) painted over a FULL-FRAME tonemap, not unpainted clear behind a
      shrunken quad. Proven twice without a headset - the Nexus mod is a one-byte SWF
      edit zeroing that sprite's PlaceObject2 scale, and a framedump inside the letterbox
      shows the tonemap at full 2048x2048 with a textureless 29-vertex flash draw after
      it. So the unsqueeze was undoing a squeeze that does not exist; `blit::stretch_band`
      and `vrcine unsqueeze` are DELETED. Shipped: `hud::on_draw` gained a Skip verdict,
      `vrcine bars hide|show` (default hide), and - because suppression would otherwise
      blind the pixel watch that detects the bars - the bar draw itself is now the primary
      cinematic signal, with the watch kept as an independent cross-check.
      *Also retired: the three failed in-headset rounds were confounded twice over (all at
      1920x1080, where the claim leaves ~12 deg of permanent black band inside a Quest 3
      eye; and the only evidence the stretch ran was a process-lifetime one-shot log).*
      **In-headset verdict pending.**
- [x] **Suspend hands/aim/laser drives during cinematics** - DONE session 29: the drives
      turned out to be suspended ALREADY, but only as a side effect of `vrDriving` going
      false with the head drive - an accident, not a contract, and `authored+look` breaks
      it by driving the head again. Explicit gates now in `hands.cpp` and `aim.cpp`, plus
      `bones::release()` on the cinematic edge (stopping the drive is not the same as
      handing the skeleton back: `reapply()` repaints for another 100 ms while clearing
      the dirty flag, and `restore_hidden()` only ever ran from inside `drive()`).
      Shipped with `vrcine drive off|authored|authored+look`. **In-headset verdict
      pending; the sticky-state half of the diagnosis is unconfirmed flat (no XR session
      means no bone drive to leave anything behind).**
- [x] **Aim dot on the verified aim ray (stage 3)** - DONE session 29: `vraim dot on|off`,
      default OFF, with distance and size sliders, persisted. Placed from the FINAL
      fire-seam ray point via a new `game_point_to_xr` (the exact inverse of
      `xr_pose_to_game`'s affine position half), so dot == shot by shared DATA rather than
      the laser's shared algebra. Published only when `ray_for()` would succeed, so the
      dot doubles as proof that the fire substitution is live. No trace exists in the mod,
      so the distance is a slider - same shape and same limitation as BioVRDev's dot.
      **In-headset calibration pending.**
- [ ] Better overlay/config UI (user's call 2026-07-27: current UI is good - this is polish
      only: grouping, naming, hiding the debug-only controls behind an advanced toggle)
- [ ] **World/viewmodel scale SPLIT (parked here 2026-07-27, session 16 part 3, user's
      call):** worldScale now defaults 100 (the user's in-headset calibration - viewmodel
      size AND distance finally read right; the world reads ~half size in trade, judged
      acceptable). If the world scale ever needs to move independently, the viewmodel needs
      its OWN stereo basis: per-eye bone offsets giving the hand cluster its own IPD so the
      gun keeps its size while reading at the true hand distance (the naive two-slider split
      puts the gun at double the hand's perceived distance and doubles hand motion - the
      session-11 percepts). Design sketch in the session-16 part-3 conversation; the per-eye
      write path (bones reapply) already exists.
      *RE-CONFIRMED POLISH 2026-07-28 (session 21 part 3, user's call: "not that important
      for the gameplay"): the ask is now two independent sliders (world + hand/model scale).
      New candidate routes since the fg-scene decode - the fovA per-rig zoom (once its
      world-coupling is explained and masked) or DrawScale on a fova-matched rig - see
      STATUS "POLISH / POST-POLISH" for the probe order.*
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
- [ ] Config surface cleanup + release-notes refresh (the FIRST release ships in M8's quick
      phase; this is the polished follow-up)
- [ ] **Done when:** a non-developer installs from the current release zip and plays with
      full HUD and no developer knowledge.

## M10 - BioShock 2 Remastered adapter (~2–4 sessions)

- [x] `src/game/bioshock2r/` adapter: new patterns.cpp; expectation is near-total core reuse
      (landed session 24 - patterns + camera + adapter; the CalcView seam is ProcessEvent-based
      because BS2 inlined the event dispatch, see docs/bioshock2/ENGINE_NOTES.md)
- [x] **Done when:** M3-level (6DOF mono) within one session of scan work; M4-level stereo within
      the milestone. Every core/adapter seam leak found → ARCHITECTURE decision log.
      (M3-level: session 24, one session as budgeted - flat 6DOF integer-exact, in-headset
      PASSED (fisheye/world-drag = the expected FOV-claim gap); seam-leak inventory in the
      decision log. FOV readback/write: session 25, COMPLETE - HorizontalFOV derived fresh
      at UShockUserSettings+0x4C, claim == rendered, vrfov/gfov default OFF, the native-path
      check killed the entire BS1 fg-porting question (viewmodel follows the world FOV
      natively), and the in-headset acceptance PASSED same day: fisheye gone, world-drag
      gone, restore edges exercised.
      M4-level stereo: session 26, COMPLETE and IN-HEADSET ACCEPTED (user, same day:
      "looks awesome - very good for everything"; world scale fine at the default 100).
      One deferred blemish the user volunteered: the viewmodel/hands read weird in
      stereo, same class as BS1's - parked to its own milestone (leads in STATUS next
      steps). Derivation: substrate derived in one session
      (UGameEngine::Draw 0x4EE8D0 via the live kick/kick2 samplers + offline capstone
      walks), and the policy gate paid out its biggest win yet: SequentialReentry runs
      on the THREADED substrate - no 1t, no flush-point, no drain guard, none of BS1's
      single-threading machinery ports. Pulse/continuous/stereo all flat-green:
      presents/s == 2 x draws/s exact, per-eye camera delta IPD-exact (6.30 UU), 2nd-pass
      CalcView replay 655/655, zero faults; `vrstereo on` one-toggle READY; every lever
      default OFF; pass 2 deny-by-default on the single gameplay caller.
      M10 is DONE: both acceptance clauses met (M3-level in one session of scan work,
      M4-level stereo within the milestone, in-headset verified). Carried forward, not
      blocking: the stereo viewmodel blemish, a user-driven load-crossing pass, and a
      combat-scene perf profile.)
- [x] **Session 32 - BS2 resolution lane + lens verdict.** `vrres` ships on BS2 (writes
      `Shared.ini [SharedOptions]`, NOT the `[WinDrv.WindowsClient]` keys BS1 uses - BS2 ignores
      those), verified end to end through a relaunch. The frozen engine-pitch bug is fixed
      (`publish_pitch_error`), and `vrinput` is dispatched on BS2 for the first time. BS2's cb0
      ray block derived at float 16 and parameterised into both the live watch and the offline
      decoder; BS1 regression-checked bit-identical.
      **Two findings that change the plan:** (1) BS1's square-backbuffer policy does NOT transfer -
      BS2 letterboxes 2048x2048 into a 2048x1421 viewport and its projection degenerates off 16:9;
      (2) BS2 carries TWO lenses that differ AT 16:9 (world tracks the FOV option, a second is
      fixed at 60 deg), a 2.06x gain error at option 100 rising to 3.99x at 130 - the leading
      explanation for the stereo viewmodel report.
- [x] **BS2 aspect bisection** - DISSOLVED session 37: there is no aspect law to bisect. The
      letterbox was the WINDOW - the engine sizes its scene viewport to the window client, and the
      game's chromed window clamps on the desktop (client tops out at 1421 rows on a 1440p
      desktop; 2048/1421 = the "mystery ratio" 1.4413). A borderless client sized exactly to the
      backbuffer renders full-height square pixels at EVERY aspect tried (1.778/1.6/0.9348/0.9321),
      the engine follows a live client resize with its own ResizeBuffers, and resolution on BS2 is
      therefore LIVE - no relaunch. Zero user boots spent (the menu scene classifies as gameplay).
- [x] **BS2 resolution picker + automatic FOV (session 37)** - BS1-parity F10 picker
      ("RENDER RESOLUTION (applies live)"): preset combo (flat/perf/native 2064x2208/sharp/max,
      Quest-3-native class), custom WxH, MPx + auto-FOV preview per selection, Apply/Restore.
      `vrres` gained the same presets + `list` + `restore`. Apply is LIVE (borderless window
      enforcement; ini persistence + deferred re-verify against the engine's lagging
      resize-persist; stereo-armed self-heal for chromed boots). The automatic FOV was already
      shipping (`vrfov` computes the option from the headset via the inverse law, per CalcView);
      session 37 verified it at the new aspects (option 138 at native renders 107.7x111.4 deg vs
      the 108x110 eye, ClaimRatioH 0.995) and relabeled the UI. Headset acceptance in the save:
      owed (fg one-cluster with a weapon at native, HUD legibility, perceived quality).
- [x] **Identify BS2's 60-deg lens** - DONE session 32 by two in-headset FOV A/Bs rather than a
      dump: the defect worsens at option 130 and VANISHES at option 60, exactly as
      `k = tan(option/2)/tan(30)` predicts. It is the viewmodel. Session 25's "foreground follows
      the world FOV natively" is retracted; BS2 has a fixed fg lens like BS1.
- [x] **BS2 viewmodel lens match (USER PRIORITY 2)** - DONE session 33 and ACCEPTED IN-HEADSET
      (*"it worked, the weapon was not moving anymore"*). The field is `PlayerController + 0x694`,
      a float in DEGREES; the live world FOV is written into it every CalcView. Shipped default ON
      with an F10 overlay toggle. Acceptance came from full frame dumps - ONE cluster at option
      100/130/80 and TWO when disarmed - because `lenses=1` turned out not to be proof of
      anything. The `0xAECACF` lead was wrong (two call sites of the same draw dispatch);
      `+0x690` is the WORLD lens and must not be written.
- [x] **BS2: the rig eats the view - RESOLVED across sessions 36-37.** The helmet half: hidden
      by default since session 36 (edge-of-FOV placement impossible for a single mesh at that
      distance; the checkbox in FILL THE VIEW is the A/B). The black-bars half was the FOV
      deficit, closed by session 37's picker + automatic FOV (accepted in-headset 2026-08-03:
      "the FOV is filling the screen").
- [x] **BS2 teardown crash on stereo-armed close - RESOLVED session 38, and the premise was
      wrong**: the close-time fault (`+0x4FF0FE` display-apply null read) is the GAME'S OWN
      exit bug - it fires with every mod hook skipped (`BVR_SKIP` bisect run G4) and vanilla
      merely hides it (CSERHelper eats the fault, 0.1 s teardown CPU). Stereo was never the
      cause. Fix: teardown-aware crash handling (WM_CLOSE/WM_DESTROY noted by the overlay
      WndProc -> faults get one log line, no dump, immediate TerminateProcess) + adapter
      hygiene gates + drain-guard poison hardening. Closes now 0.1-0.3 s, zero dumps -
      faster than vanilla. Accepted: 3 armed sim closes + 5-min armed soak, AND the
      in-game quit from gameplay with save-before-exit, flat (save written, WM_DESTROY
      noted, zero dumps; a teardown-gate deadlock was caught and fixed in the same round,
      plus a 15 s exit watchdog). No user half remains. Full derivation: ENGINE_NOTES
      session 38 + wrap-up round.
- [x] **VR PACING (BLOCKER - the game is unplayable in VR)** - an OpenXR session that is running
      but not FOCUSED paces the game at the runtime's not-visible cadence (~10 Hz). Alt-tab
      reproduces it. Not a block (`lastWait 0 ms`); the frame HANDOFF is what paces. Session 28's
      "keep submitting while unfocused" must be preserved - the fix is to stop WAITING, not to
      stop submitting.
      (DONE across sessions 34-36: session 34 moved the wait off the present thread and added the
      detach lever; session 36's first real VDXR attach then showed detach STRANDS the headset -
      VDXR never re-promotes empty keepalives - so BS2 ships detach OFF and self-heals on refocus,
      the unfocused frame loop measured cheap (lastEnd ~1 ms at VISIBLE). Residue queued in STATUS:
      keepalives that carry real layers, so detach-on gets recovery too.)
- [x] **BS2 vrstereo FREEZE (RELEASE BLOCKER, sessions 34-36) - RESOLVED; full-rate stereo ships
      on `reentry 1t`.** Root cause (session 35, verified live session 36): Draw's tail calls a
      render flush point (0x69FC30) whose threaded branch is a latch-test-then-Wait(INFINITE);
      the doubled draw raced it and the lost wakeup wedged the game in 5-100 s. Measured session
      36: the wait was entered on EVERY doubled frame at every resolution (`wait2/s == 2nd/s`) -
      the resolution/FOV work never created reachability. Fix: BS1's session-8 cure duplicated
      with fresh constants (drain guard + flush-point force; quotient never poked), ~15% draw
      cost. Accepted: 10-min decider soak in the user's save, user-driven load-crossing matrix
      (stereo sticky, guardskips 0), and an immersive in-headset run of full-rate stereo on 1t.
      Full derivations: ENGINE_NOTES "The render flush point"; history: FREEZE_HANDOFF.md.
- [ ] **BS2 pitch-servo sign check in-headset** - the one thing flat cannot answer about the
      session-32 fix.
- [ ] **BS2 swing-to-attack** - one `swing::publish_gate` line plus the melee-equipped predicate
      (the hard part; prefer the ProcessEvent-by-name seam). Unblocked now that `vrinput` reaches
      core's flat test suite on BS2.

### M10.1 - BS2 motion controls (session 39; M6/M7-parity, derived fresh)

- [x] **Dispatch verdict** (session 39): GetPerfectFireStart is native-to-native (probe:
      fire-watch on Lane-A FName globals + full PE census, GNames 0x1A614D0 fresh);
      InitiateDamage is PE-visible 1:1 with fires - the by-name timing anchor.
- [x] **Decoupled aim, flat-proven** (session 39): both impls hooked (weapon 0x89DCB0 via
      vtbl-slot census - one body for the whole family incl. the drill; ability 0x81CE80
      via targeted sweep), out-param rotator substitution moves impacts with the camera
      provably static - "look left while shooting right" on BS2.
- [x] **XR hand ray drives the seam** (session 39): b2r frame_context (xr_local_trim_quat
      algebra), 1:1 rotator substitution (25.00 deg delta exact), 250 ms freshness gate.
- [x] **Laser + aim dot** (session 39): compositor quads under SR stereo; dot published
      from the final fire point, round-trip error 0.0000 UU.
- [x] **The rig rides the controller** (session 39): AHands -> SkeletonInstance +0x430,
      64-bone pose bank poke-proven; rigid cluster drive, NO lock domain; coupling
      acceptance aimRayMaxDevDeg constant (0/0/0/0.02/0), write-loc at exactly 100 UU/m.
- [x] **Cheats lane** (session 39): F9=GiveAll by effect; digit-key weapon switching flat.
- [x] **input_drive port** (session 40): UpdateInput IS in the binary but orphaned (viewport
      vtbl slot 73, zero callers) - pumped per present + SetUseController through the client's
      own slot 73, all RVAs fresh (GEngine ptr 0x1A638F0, IAT slot 0x1C0DBFC). Flat-proven:
      sticks walk the player ~400 UU, a synthetic trigger fires through the seam, the main
      menu navigates on the dpad, and the engine's own UI switched to controller prompts.
- [x] **Per-hand clusters** (session 40): bone-name map auto-detected at
      SharedSkeletonData+0xB4 (64/64 named) -> left = wrist 7 + fingers 8..28 + pivot 62,
      right = wrist 36 + fingers 37..57 + pivot 63 (the weapon attach, proven by driving it
      alone). Each cluster tracks its own controller: 35.0 UU on the moved hand, 0.0 on the
      other, 120.0 UU separation for 1.2 m apart.
- [x] **Model alignment + scale + bullet origin** (session 40 - the first look's top three
      defects): the composition was DISCARDING the anchor's authored frame; fixed, so the
      mesh-vs-authored angle at rest is 0.21 deg (was ~81.6). `vrhands scale` scales about
      the anchor (anchor invariant to 0.00 UU) and is independent of worldscale. Bullets
      leave the hand (61.9 UU displacement, clamped at 200).
- [x] **Dual lasers + dual dots** (session 40, user's call - BS2 is natively dual-wield):
      both hands render a beam and a dot, 11 of the 16 compositor layers, each beam
      terminating at its own dot so there is one bright point per hand.
- [x] **Aim/model sliders + presets** (session 40): F10 "HANDS + AIM (per hand)" panel with a
      tuning-hand radio; 14 new vrpreset keys (a fresh boot loads 22 values, was 8).
- [ ] **In-headset acceptance**: aim-follows-controller + model-in-tune-with-laser (the
      session-21 "perfect" bar), at the user's save.
- [ ] **Ability seam live check**: BLOCKED on a projectile plasmid, not on the seam.
      Telekinesis provably does not traverse GetPerfectFireStart (two casts at a grabbable
      object, abi stayed 0), and `<X>BasicPlasmid` exists ONLY for Telekinesis in the exe -
      the other item class names are in the content packages and still have to be found.
- [ ] **Per-weapon aim presets** (deferred out of session 40, deliberately - was bundled into
      the sliders box): needs holdable identity under session-21's rules (key off the rig's
      live holdable, never vtable-gate it, unresolvable clears, never seed before a value
      source exists), and no tuned value source exists until the user calibrates in-headset.

## Post-v1 backlog (not scheduled)

- **Selection wheels** (moved here 2026-07-27, session 16 part 4, user's call - the
  existing switching UI is good enough for v1): controller-anchored quad wheels for
  weapons (right grip) and plasmids (left grip), HL:Alyx-style hold-flick-release;
  inventory via adapter queryState, select via console exec / synthetic input, haptic tick.
- Asymmetric per-eye projection matrices (reclaim wasted pixels)
- Wrist-anchored HUD elements (health/EVE)
- **Hand IK arms** (user-requested post-polish focus, 2026-07-25): articulated forearm/elbow
  that bends based on the controller hand position, instead of M7's rigid hands+stub. Two-bone
  IK (anchor a shoulder, hand at the grip pose, solve the elbow); the game provides no full arm
  rig so this means fabricating/grafting one. TRADEOFF to weigh first: a slightly-wrong IK arm
  (elbow poking the wrong way, body clipping) reads worse than M7's floating hands+weapon, so
  this is opt-in polish - only ship it if it actually looks right. Pairs with two-handed
  weapons + physical melee swings.
- **Left-handed mode** (user's call 2026-07-28): a `handedness left` toggle swapping
  every ROLE assignment in the mod's own mapping - the weapon viewmodel/aim/laser
  follow the LEFT controller, plasmid the right, triggers/grips/radials/ammo modifier
  swap with them, optional second toggle for stick swap (move on right stick). The
  per-hand tuning (trims/offsets) must follow the ROLE, not the physical hand.
  Accepted cosmetic compromise: the visible weapon hand stays the RIGHT-hand mesh
  (mirroring is a wall - negative bone scale hits the session-16 attach-path
  inverse-scale minefield and flips triangle winding; re-attaching to the left hand
  has no grip art). The input/pose swap is what handedness is actually for and is
  roughly a session of work incl. testing.
- **Seated / standing recenter modes** (user's ask 2026-07-29, session 23 - deferred to the
  next release): the mod menu has ONE recenter. It zeroes yaw AND full head position, so
  whatever posture you recenter in becomes the character's eye height - correct for that
  posture, wrong the moment you change it. Standing players want a second mode. Three
  candidate designs, undecided:
  (a) **yaw + horizontal only** - do not zero height, so real standing height drives eye
      height and crouching maps 1:1; seated keeps today's zero-everything behaviour.
      Smallest change, no new reference space, no guardian dependency.
  (b) **floor-referenced** - `XR_REFERENCE_SPACE_TYPE_STAGE`/`LOCAL_FLOOR` for standing.
      Most physically correct, but the mod currently creates only LOCAL
      (`openxr_runtime.cpp:720`) and the whole aim/body calibration sits on that pose
      pipeline, so it is the riskiest.
  (c) **height offset preset only** - one recenter plus a saved height boost that seated
      uses and standing sets to zero. Purely additive, but manual.
  Whichever wins persists in `vrpreset.ini` and gets an overlay control next to Recenter.
- BioShock Infinite (UE3 build 6829) adapter feasibility study
- OpenVR backend (if some runtime needs it)
