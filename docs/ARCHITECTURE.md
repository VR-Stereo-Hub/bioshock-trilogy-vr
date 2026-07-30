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
run (progressive enhancement - this is how the BioShock 2 port stays cheap and how an Infinite
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
- Core never touches UObject/FName - core speaks poses, meters, and D3D11. Adapters own all
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

1. **MonoScreen** - game frame on a quad ("cinema screen"). Validates all OpenXR plumbing with
   zero engine knowledge.
2. **MonoTracked** - same image to both eyes of a projection layer, camera driven by HMD 6DOF
   via CalcView, FOV forced to headset FOV. Already a big experience win; validates camera math,
   world scale, prediction timing.
3. **AlternateEye** - camera alternates ±IPD/2 per game frame, each frame submitted to one eye
   (stale image held for the other). Judders; not shippable; proves geometric stereo correctness
   in ~a day of code before the big bet.
4. **SequentialReentry (primary bet)** - hook the scene-draw entry (found via RenderDoc callstack
   on a world draw call), then per frame: set left camera+FOV → call original → copy backbuffer →
   set right camera → call original → copy. The engine computes every view-dependent effect
   natively per eye, so the per-shader fix long tail mostly evaporates. The renderer is
   single-threaded by default (`UseMultithreadedRendering=False`) - one linear call graph.
5. **DepthReproject (fallback)** - vorpX-Z3D-style synthesis of the second eye from color+depth.
   Full framerate, edge artifacts, flat-ish. Ships if re-entry hits an intractable wall.

Rejected as primary: 3Dmigoto-style draw-call duplication + vertex-shader stereo displacement -
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

- **Lane 1 - synthetic XInput** (early, permanent fallback): OpenXR actions composed into
  XINPUT_STATE inside our proxy. Sticks = locomotion, trigger = fire. Zero engine hooks; full
  playability and menu navigation from M5.
  *As-built (M5 session 9): the "zero hooks" premise half-survived contact with reality - we
  still never hook the game, but the Steam overlay eats calls routed through the proxy export
  and the remaster never calls its own pad-read path in windowed mode, so the shipped shape is
  core/input/xinput_bridge (compose + merge + game-IAT wrapper) + core/vr/openxr_input (action
  set) + game/bioshock1r/input_drive (drives UWindowsViewport::UpdateInput and the engine's own
  SetUseController). Details: ENGINE_NOTES "Gamepad architecture".*

### Controller mapping (Quest 3 Touch -> Xbox 360 pad, M5; face buttons re-routed session 19)

The game's own layout (User.ini XENON_*, ENGINE_NOTES session 19) is A=Use,
B=Heal, X=Reload/Hack/EVE, Y=Jump - so the XR layer re-routes the face buttons
to VR conventions (jump on the lower-right button) instead of passing through.

| Touch input | XInput output | BioShock meaning |
|---|---|---|
| Left thumbstick | LS | move |
| Right thumbstick | RS | look (Y zeroed during VR gameplay - the HMD owns pitch; `vrinput pitchkill off` restores) |
| Right stick, CLICK HELD + push up/down/left | DPAD_UP / DOWN / LEFT pulse | ammo-slot select (each dpad direction selects its slot; turning suppressed while held; grips suppress it - the radials read the stick) |
| Right stick click alone | (nothing) | zoom is REMOVED in VR (user's call - an HMD FOV zoom is a comfort hazard; RS click never reaches the game) |
| (while a grip/bumper is held) | RS Y passes through | the radial wheels read stick Y for selection - the pitch kill lifts for the hold, and the game side snapshots/restores the PC pitch around it so wheel-time look drift cannot stick |
| Right trigger | RT | fire weapon |
| Left trigger | LT | fire plasmid |
| Right grip (squeeze, 0.70/0.55 hysteresis) | RB | next weapon / weapon radial on hold |
| Left grip (same) | LB | next plasmid / plasmid radial on hold |
| A | A | use / interact / menu confirm (headset verdict: A stays use) |
| B | Y | jump |
| X | X | reload / hack / inject EVE |
| Y | B | first-aid (med hypo) |
| Left stick click | LS click | crouch |
| Left menu, short press (<500 ms, pulsed on release) | START | pause menu |
| Left menu, hold (>=500 ms) | BACK | map/objectives |
- **Lane 2 - engine-level**: console-exec dispatcher for discrete actions (weapon/plasmid
  switch, ToggleHUD, SetFOV - one high-value hook makes dozens of features one-liners); direct
  hooks for continuous aim.
- **Decoupled aim**: UE2.5 fire traces derive from player view rotation, so camera hooks aren't
  enough - hook the aim source used by fire logic and return the controller aim there while
  CalcView keeps returning the HMD pose. Right hand = weapons, left hand = plasmids.
  *As-built (M6 session 10): there is no single "GetPlayerViewPoint". Attacks are ABILITIES, and
  each fire path asks its own `GetPerfectFireStart` (AWeapon for guns and melee, UAttackAbility
  for plasmids) for where the shot starts, after which the engine applies its own spread. Those
  two C++ implementations are the seam (`game/bioshock1r/aim.cpp`, command-gated `vraim`) -
  hooked at the implementation, not the UnrealScript exec thunk, because native callers bypass
  thunks entirely. Hand attribution is object identity seeded by the trigger the bridge itself
  composes, so switching weapon or plasmid re-learns for free. The plasmid path's trace
  DIRECTION lives one layer deeper (the damage factory) and is the open piece; addresses and
  live findings in ENGINE_NOTES "Fire flow / aim".*
- **Menus (gameswf Flash)**: menu mode = whole frame on a quad + controller laser → virtual
  mouse into whichever input path the menus actually read (DR-6 determines: DirectInput vs
  window messages vs cursor pos). In-game HUD later via draw-call capture → floating quad (M9).

## VR runtime

OpenXR-only (`XR_KHR_D3D11_enable`), session created on the game's own device - serves VDXR
(Virtual Desktop) and SteamVR (Steam Link) with one backend. The `IVrRuntime` interface leaves
room for: an OpenVR backend (if ever needed) and the **DR-1 fallback**: a 64-bit companion
compositor process owning the OpenXR session, fed eye textures via D3D11 shared handles and
poses via shared memory - only built if 32-bit OpenXR clients turn out unsupported by a needed
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
- **2026-07-23 · Hand-rolled logger, no spdlog** - one less dependency; needs are trivial
  (timestamped lines to one file).
- **2026-07-23 · IGameAdapter introduced incrementally.** DR-4 ships `capabilities`/`init`/
  `setFov`/`drawDebugUi` only; `onCalcView`, `execConsole`, aim/hands/re-entry land with the
  milestone that consumes them (M3+). Each deferred method needs type design (`CameraOverride`,
  `Pose`, `GameState`, ...) that the consuming milestone should inform - stubbing them now would
  be speculation. The interface is in-process only, so growing it later is a one-line change
  per adapter. The full target shape stays documented above.
- **2026-07-23 · M3 pose access is pull-based, not push.** The adapter's CalcView detour calls
  `vr::get_head_pose()` / `vr::vr_camera_mode()` (game -> core calls are allowed; core still
  knows nothing about the game). The ARCHITECTURE `onCalcView` push seam is deferred until
  core actually needs to sequence the call (e.g. per-eye offsets in M4); pulling avoids a
  pose-plumbing layer while there is exactly one consumer. Conversion conventions (XR right/
  up/-fwd meters -> UE fwd/right/up UU, FRotator signs) live in `camera.cpp` next to the math.
- **2026-07-23 · Adapter-drawn debug UI through the seam** (`IGameAdapter::drawDebugUi`). The
  overlay calls it via the interface; all engine-semantic UI (Unreal units, FRotator degrees,
  FOV offsets) stays inside `game/bioshock1r/`, next to the hook state it displays - which also
  keeps the atomics' thread-safety surface in one file. `core/ui` includes only the seam header
  (no addresses, no engine knowledge), so the layering rule holds. Rejected: a shared POD debug
  struct in core (leaks engine semantics) and overlay including bioshock1r headers directly
  (rule violation).
- **2026-07-23 · Static CRT (/MT)** - no VC redist dependency inside the game process.
- **2026-07-23 · MIT license** - compatible with every dependency (BSD-2, MIT, Apache-2.0);
  matches the free-open-injector legal posture for 2K-game mods.
- **2026-07-23 · AlternateEye via held stale images, not blanking.** Each eye gets its own
  backbuffer-sized swapchain; a frame is copied into the CURRENT eye's swapchain and that eye's
  located pose is stored with it. Submission always references both swapchains' most recently
  released images (spec-legal without re-acquiring) with their STORED poses, so the compositor
  reprojects the half-rate-stale eye instead of it going black - judder, not flicker. The eye
  sign for the game's camera shift is published at Present-tail AFTER submit and only flips once
  a frame carrying the current eye's offset was actually captured (the sign itself is the
  attribution: the value CalcView consumed is the offset baked into the next backbuffer we see).
  The un-offset frame right after enabling therefore flows through the mono path instead of
  being mislabeled as a left-eye image. A "Swap eyes" diagnostic checkbox negates the sign to
  disambiguate inverted depth (i.e. deeper pipeline buffering than the 1-frame assumption) from
  other geometry errors in-headset. Rejected: blanking the stale eye (flicker), acquiring both
  swapchains every frame (pointless copies), per-eye fov storage (fov only changes on user
  toggles; one-frame mismatch invisible).
- **2026-07-24 - SequentialReentry stereo (M4 rung 2): eye pair per game tick via the scene-build
  double-call.** Three coupled choices. (1) *Coherent pair by replay*: pass 1 (LEFT) runs the full
  CalcView drive, caches the final un-eyed camera (game-thread-local, no locks), applies -IPD/2;
  pass 2 (RIGHT) replays the cached base + IPD/2 instead of re-sampling the head pose - a Present
  lands between the two passes, so re-sampling would skew the pair (vertical disparity). (2) *Eye
  attribution by SPSC tag ring* (`vr::sr_push_eye`): the game thread pushes the eye sign at each
  nested engine submit; Present-tail pops one per Present - valid because submits:presents are
  exactly 1:1 in gameplay (live-verified); depth > 2 self-clears (mode-boundary skew); presents
  without tags flow mono, so AER/mono paths are untouched. Rejected: presenting-thread inference
  from present parity (breaks on any dropped present) and cross-thread mutex handoff (a lock in
  the present path). (3) *Pacing by drain-poll, not the engine's waiter*: before pass 2, poll the
  frame-id pair's completion high bits until the pipeline is empty (bounded 20 ms, skip on
  timeout) - the engine's own event wait has a live-proven lost-wakeup race (two hangs); a poll
  cannot lose a wakeup, and with zero frames in flight the racy waiter never engages. The timeout
  doubles as the unfocused path: presents stop, doubling degrades to mono instead of stalling.
- **2026-07-24 - Single-threaded substrate by hw-thread-count poke, not launch arg, flush
  reimplementation, or client-bool poke.** The stereo double-render needs the renderer inline on
  the game thread (the threaded pump protocol deadlocks under doubling and crashes on empty
  wakes - session 6/7 evidence). Chosen: `reentry 1t on` pokes the engine's hardware-thread
  numerator (patterns.h kNumHwThreadsRva) to 1 AFTER arming the drain empty-slot guard - the
  flush-point's decision chain then selects its own native inline branch every frame, on the
  engine's own code path. Rejected: the `-onethread` launch arg (not parsed by the remaster -
  proven by string absence + a live pump thread with the arg on the command line); MinHook
  reimplementation of the flush-point's inline branch (faithful-copy risk, and the submit's pump
  kick would race our inline drain); poking the per-client "use render thread" bool at
  [[scene+0x3DC]]+0x4C (heap object, must be re-found per boot; the static numerator is a
  10-reference single-purpose pair consumed by seven copies of one test); poking the GIsEditor-
  class global 0x1375BD4 (500+ refs, crashed loads - recorded dead end). The runtime poke leaves
  the pump thread parked (submit stops firing in inline mono, so it is never kicked; any stray
  wake hits the guard) - a boot-time poke that prevents pump creation entirely is queued as
  polish, not correctness.
- **2026-07-25 - Synthetic gamepad: last-hop IAT injection + driving the engine's own pad
  path.** The proxy post-hook seam alone was insufficient twice over: the Steam overlay
  code-hooks the export thunk of every loaded xinput DLL (calls die inside Steam Input before
  the proxy body runs), and the remaster never calls UWindowsViewport::UpdateInput - the only
  pad-read in the image - in windowed mode (boot-time GetState probe only, no re-probe ever).
  Shipped shape: (1) core/input/xinput_bridge composes synthetic state (XR slot + self-expiring
  seam test slots, buttons OR / triggers max / larger-magnitude axes, bridge-owned packet
  counter) and re-points the game's IAT ord-2 slot at its wrapper, keeping the previous target
  as passthrough so Steam-served real pads keep working; (2) game/bioshock1r/input_drive arms
  the engine's own UWindowsClient::SetUseController(TRUE) (UI prompts + game-level gates flip
  properly) and calls viewport->UpdateInput(0, dt) once per present from the CalcView detour,
  SEH-guarded, so the STOCK pad pipeline consumes the composed state; (3) core/vr/openxr_input
  is a sibling .cpp fed handles at five runtime lifecycle points (no globals exposed, runtime
  file stays display-only). vrinput enable persists via a marker file read at DLL attach
  because the client-init probe runs before the command seam's first poll. Rejected: always-on
  connected reporting (violates passthrough-when-off), WM_DEVICECHANGE nudging and the
  UseJoystick/UseController ini keys (all provably ignored), MinHooking UpdateInput (nothing
  calls it - there is nothing to hook), and dpad chords on Touch (hidden state; the M8 radial
  wheels own discrete selection).
- **2026-07-25 - Aim seam: hook implementations, and let the engine keep its spread.** Three
  findings shaped M6. (1) The engine ships a readable symbol table for name-based natives
  (registration string `int<Class>exec<Func>` -> `.data` entry -> impl pointer), so
  `pattern_scan::find_native_function` resolves aim symbols with no hardcoded addresses and no
  prologue scanning - and dumping that table offline is now the first stop for any engine
  question. (2) Those `exec` thunks are script-entry only: hooking all four aim thunks caught
  ZERO calls while shooting, because C++ callers go straight to the implementation. The shipped
  seams are therefore the implementations, resolved via the RTTI-derived vtable slot (weapon) or
  a prologue-checked RVA (ability). (3) Substituting at `GetPerfectFireStart` rather than at the
  spread function keeps per-weapon accuracy: the engine applies `ApplyAimError` to our direction
  afterwards, so a shotgun still spreads. Substitution is value-driven (each out-param the
  engine filled with a position gets the hand's origin, each direction gets the hand's
  direction, zeros are left alone) because the same function's out-param order differs between
  the two signatures. Ownership gates read the weapon's owning pawn / the ability's instigator
  and compare against the AShockPlayer vtable - the same check the engine makes itself - so AI
  fire keeps its own aim. Rejected: writing the pawn/controller Rotation field (it also drives
  movement direction and pawn facing, and cannot give the two hands independent aim), and
  hooking `APawn::GetViewDirection` (live-proven never called during a shot).
- **2026-07-25 - M7 viewmodel: write the actor's transform from the CalcView detour, and
  put the laser in the compositor rather than the scene.** Two choices worth recording.
  (1) The hands were expected to need a placement hook - the engine positions the viewmodel
  every tick, so the plan budgeted for finding that code and landing after it. It turned out
  the plain field write already wins: CalcView runs after the engine's tick placement, so
  `hands::on_calcview` is the last writer of the frame. The rejected alternatives (hooking
  the AHands Tick vtable slot, or disassembling the writers of actor+0x1D8) stay unbuilt
  because a simpler thing works; if a future engine change breaks the ordering, they are the
  fallback. The write uses the GRIP pose while the fire ray keeps the AIM pose - grip is
  where the hand physically is, which is what a model wants, and aim is the runtime's own
  pointing ray, which is what a bullet wants. (2) The laser is XR quad layers, not geometry
  injected into the game scene: layers are per-eye correct for free, need no engine hook, no
  render-state interaction and nothing the game's renderer can clip or depth-test away. The
  cost is that it is invisible to flat testing - quad layers exist only inside the
  compositor - which is a real verification gap, accepted because the alternative trades it
  for engine coupling in the hottest path we have. The dots take their trim from aim.cpp
  rather than owning a copy, so the beam and the bullet cannot drift; a laser that disagrees
  with the shot is worse than no laser. Deferred by the user: a dot at the true IMPACT point,
  which needs a callable per-frame line-check that the engine only ever runs mid-shot.
- **2026-07-26 - M7 rebuild: drive the viewmodel at the BONE level, not the actor.** After
  the first two in-headset runs the actor-pinning approach is retired as a dead end, by the
  user's design review. It cannot deliver the goal ("weapon and plasmid hand each one with
  its own controller") for three structural reasons, all live-proven: the actor's pivot is
  the eye anchor with the mesh a metre out, so every rotation swings on a lever; both arms
  are ONE skinned mesh on ONE actor, so a single transform can never decouple left from
  right; and the equipped weapon renders from its attachment matrix, ignoring its own actor
  fields. The replacement is a two-level scheme: (1) identify the viewmodel's draw calls,
  which also reveals whether the sleeve and hand are separate material sections - if they
  are, hiding an arm is a draw-call skip and costs nothing; (2) reach the skeleton's bone
  matrices and write the hand bones directly from the controllers, collapsing the forearm
  bones to hide them. Writing bones rather than patching render matrices is a deliberate
  choice, not a convenience: the engine recomputes attachments from bones, so the plasmid's
  hand FX and the weapon's muzzle effects follow for free, whereas a render-time matrix
  patch would leave them floating at the old position. Render-side patching is reserved for
  what has no engine-side handle - model scale (no DrawScale field has been found) and the
  viewmodel's separate projection. Explicitly OUT of scope by the user's call: elbow IK,
  arm bending, two-handed grips, and BioShock 1 dual-wield (the engine equips one hand at a
  time; that fight belongs to the BioShock 2 adapter).
- **2026-07-25 (evening) - M7 placement: one ray, local-frame trim, and the two walls we
  hit.** The first in-headset run failed on three compounding defects; the fixes define the
  contract going forward. (1) ONE RAY: the model aligns to the AIM pose plus the same trim
  the fire ray and laser use - grip pose demoted to an option. Anything visible that claims
  to point where shots go must be derived from the identical ray, never a sibling copy.
  (2) Rotation offsets are quaternions composed in the controller's local frame; euler adds
  after conversion are banned (only correct at one orientation - live-proven "goes crazy").
  (3) Two hard engine walls, both live-proven and recorded in ENGINE_NOTES: the renderer
  draws ATTACHED actors from the attach matrix (so driving the weapon actor directly is
  inert until a Base-detach experiment), and actors cull by ORIGIN (so pivot correction via
  position offset is bounded by the hand's distance from the face). Rejected along the way:
  shipping the measured pivot correction as defaults (-100 cm forward = origin behind the
  camera = invisible rig).
- **2026-07-25 - M6/M7 split stays as planned.** M6 is the aim vector only. The wrench turned
  out to damage through a Havok collision phantom rather than a trace, so "melee feels aimed" is
  purely a hands-rendering matter and belongs to M7 with the visible weapon; articulated IK arms
  remain post-v1.- **2026-07-27 - Viewmodel strategy pivot: patch the foreground pipeline's INPUTS, stop
  countering its outputs (user's call after three sessions of counter-modeling).** The
  render-lock lane (model the fg transform, move bones so wrong math lands right) kept
  passing flat acceptance while the in-headset percept did not move; every session
  surfaced one more unmodeled term. Session 15 replaced it: the fg pass's FOV is a live
  PlayerController field (consumed per frame - ENGINE_NOTES session 15) and `vrfgfov`
  writes the world-equivalent spec, making the rig render through the WORLD lens with no
  model at all. Two constraints found the same night bound the remaining work: the fg
  eye dollies back by a fov-coupled amount whose source is still unfound, and bones can
  never counter it at the matched lens because the engine culls content behind the world
  camera. Fallback ladder if the dolly source stays unfound: (a) accept the offset with
  matched lens + exact laterals; (b) the vm_draw replay lane (render the rig ourselves at
  the controller, engine rig hidden - the user's original proposal, still on the table).
- **2026-07-27 (session 17) - M7.5: transfer the head-look yaw into the body every frame,
  and let the RECENTER REFERENCE absorb exactly what the body took. This OVERTURNS the
  M6-era "rejected: writing the pawn/controller Rotation field" note above.** That
  rejection was correct for M6's question (aim) and wrong for this one (locomotion): the
  objection was that the rotation field "also drives movement direction and pawn facing",
  which is precisely what M7.5 needs it to do. The user found the root cause by playing -
  the body only turns with the right stick, so with the head turned, left-stick forward
  walks along the old facing, the viewmodel's composition frame drifts from the hand, and
  past ~90 deg the rig is culled. `body.cpp` writes `PC+0x1E8` (the controller's
  `Rotation.Yaw` - located and proven in ENGINE_NOTES session 17) once per rendered frame.
  Two design choices carry the risk. (1) **The recenter reference absorbs the transfer.**
  The camera and both halves of the controller-to-world mapping depend only on
  `gameYaw - recenterYaw`, so moving both by the same amount is a pure relabel - the
  user's hard non-regression requirement (the hand must never follow the head again)
  becomes a theorem rather than a tolerance, and the yaw path was converted to integer
  rotator units so the cancellation is exact. `on_calcview` returns the units actually
  COMMITTED, never the units requested, so the two can never drift apart. (2) **A probe
  handshake replaces a watchdog**: on arm the module transfers 1.1 deg and checks the body
  actually moved by it before scaling up, undoing itself and hard-disabling after three
  failures. A slow rolling watchdog would have let tens of degrees of camera error
  accumulate before tripping; this bounds the worst case to ~1.1 deg for one frame. The
  gameplay-view guard deliberately drops the `viewActor == pc` escape hatch that aim.cpp
  keeps for the menu attract scene - the aim ray wants the menu, a body write does not.
  Rejected: steering the body through synthetic right-stick input (native and safe, but
  the applied amount is unknowable per frame, so the recenter absorption lags by a frame -
  exactly the drift the invariant forbids); writing the pawn rotation (it does not move
  the camera, so absorbing into the recenter would counter-rotate the view every frame,
  and the engine re-stamps it from the controller anyway - live-confirmed the pawn follows
  our PC write for free).

- **2026-07-28 (session 20) - ONE trim algebra for ray, laser, and model; quat helpers
  promoted to core.** The fire ray used to apply its calibration trim as rotator ADDS in
  game space after the XR->game map, and the laser as yaw/pitch adds inside a spherical
  decomposition - while the model composed its trim as a quaternion in the controller's
  LOCAL frame. Three algebras for one physical quantity agree only at the pose the user
  tuned at; the session-20 `vraim synccheck` sweep measured up to **28.21 deg** of
  ray-vs-barrel divergence at rolled poses with identical trims fed to both chains
  (ENGINE_NOTES session 20). Now all three run the model's compose - `q_ctrl (x) q_trim`
  via `xr_local_trim_quat`, roll slot 0 for the ray/laser (roll is innermost and cannot
  move a ray), roll dropped only at the ray's final rotator write - implemented ONCE as
  pure functions in `frame_context.h` (`ray_pose_from_xr` = `model_pose_from_xr` + roll
  drop), which production and synccheck share, so the sweep measures the shipping code
  and the gate is `synccheck ~0 at every orientation`. The laser's origin-offset basis
  also adopts the ray's zero-roll convention (right built from the ray's yaw ANGLE, so
  it stays defined at any pitch - the old `d x worldUp` cross degenerated near vertical
  and silently dropped the right/up offsets there). The quat helpers moved to
  `core/util/xr_math.h` (`bvr::xrmath`) because core code (the laser) must not include
  game headers; `ue_math.h` re-exports them so game code keeps its spelling. The model's
  own full-roll position-offset basis is deliberately UNTOUCHED (live user tuning rides
  it), and tuned trim values carry over: the old and new algebras agree exactly at the
  neutral tuning pose. The legacy `vrhands aligntrim` euler coupling was deleted - wrong
  algebra everywhere but the tuning pose, and nothing left for it to do.

- **2026-07-28 (session 21): per-weapon profiles are a SWAP LAYER over the
  existing R-hand atomics, not a lookup at ray build.** The alternative
  (resolving `profile(weaponKey)` inside the hot ray/laser/model paths) would
  have threaded weapon identity through three modules and made the laser
  publish depend on a map lookup. Instead the six R-hand atomics stay the
  single live truth every consumer already reads; the profile layer only
  stashes them on a weapon-key change and re-loads the new key's values
  (seeded from current on first sight). Consequences the design accepts: the
  map lags the atomics between switches (stash points: switch + save), and
  anything that overwrites the R atomics wholesale must re-apply the active
  profile afterwards - which is exactly the preset-tail
  `aim::reapply_weapon_profile()` hook (the flat-caught clobber). Same-frame
  consistency is free because swap and consumers all run in the CalcView
  tail on the game thread.

- **2026-07-28 (session 21): every new render lever ships DEFAULT OFF.**
  `vrfgnode sync|fova` can invalidate the headset-approved session-16
  calibration (lockpull/gains encode the old fovA/fovB zoom-pull), so the
  shipping composition stays byte-identical to session 20 until the
  session-22 retune re-runs the acceptance ladder in the new regime and the
  user judges it in the headset.

- **2026-07-29 (session 23): the version string is generated, never hand-edited.**
  `BVR_VERSION` was a `#define` in `framework.h` and shipped "0.1.0" across v0.1.0,
  v0.2.0 AND v0.3.0, which made an external crash report unattributable to a release
  and cost a whole session to resolve by PE-timestamp forensics. It now comes from
  `project(BioshockVR VERSION ...)` via `cmake/GenerateVersion.cmake`, regenerated
  before every build and stamped with `git describe --tags --always --dirty`; the
  startup log also prints the DLL's own PE TimeDateStamp and an `env:` line (OS build,
  cores, RAM, host LAA). Consequence accepted: the version now lives in CMakeLists.txt
  and a release requires bumping it there, so the tag and the binary cannot diverge
  silently. Bumping it anywhere else is a bug.

- **2026-07-29 (session 23): crash dumps carry heap-adjacent memory, and the crash log
  carries registers.** `MiniDumpNormal` gave stacks and a module list only - enough to
  see THAT a worker thread jumped into game heap, not enough to see what smashed the
  slot. Now `MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
  MiniDumpWithProcessThreadData | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
  MiniDumpWithUnloadedModules`, plus a re-entrancy guard (a fault inside
  `MiniDumpWriteDump` used to recurse), the integer registers, a symbolized
  return-address chain, and an explicit read/write/DEP-execute classification of the
  access violation. `BVR_FULLDUMP=1` escalates to full memory when asking a reporter
  for one. Cost accepted: dumps grow from ~150 KB to a few MB.

- **2026-07-29 (session 23): a loading screen is not a deadlock.** The reentry watchdog's
  signature (stereo on, game thread inside a hooked call, builds and presents both
  frozen for 1.2 s) is satisfied HONESTLY by a level load, so the first load after
  VR PRESET 1 auto-disabled stereo and left the user in flat-per-eye VR until they
  pressed the preset a second time - user-reported, then reproduced flat. The watchdog
  now stands down while `hud::screen_only()` is true, and an auto-off records that IT
  was the cause and re-arms once builds and presents advance again with the game thread
  outside our detours for 500 ms. A deliberate `vrstereo off` / `reentry unhook` clears
  that flag, so the watchdog can never resurrect a state the user turned off.
  Coverage consciously reduced: the main menu also trips `screen_only`, so deadlock
  detection is inert there. Judged acceptable because the event kicks are opt-in
  (`wdkick` defaults off) and the auto-off "recovery" never actually recovered - its own
  message told the user to kill the game.

- **2026-07-29 (session 23): the ammo-select modifier is the LEFT thumbrest by default,
  with an automatic click fallback.** A thumbrest modifier is necessarily cross-hand -
  a thumb cannot rest on the pad and push the stick beside it - so the modifier is the
  LEFT pad while the slot directions stay on the RIGHT stick, leaving existing muscle
  memory intact. Two consequences the design handles explicitly: (1) a thumb PARKED on
  the pad is not a deliberate gesture, so in thumbrest mode turn is suppressed only
  while the stick is actually pushed past the select threshold, not for the whole rest;
  (2) not every controller has a thumbrest (Pico has none; some SteamVR setups do not
  report one), so the stick click keeps working until a real thumbrest touch is
  observed, after which the mapping is exactly what was chosen. Overlay combo
  "Ammo-select modifier" and `vrinput ammomod click|thumbrest|both` switch it.

- **2026-07-29 (session 24, M10): adapter dispatch moved to `game/adapter_registry.cpp`,
  selected by host exe name; host detection is SILENT by contract.** The registry owns
  `init_adapter()`/`adapter()` (moved verbatim from the BS1 adapter, same fail-soft
  semantics: publish even on scan failure so the overlay can show it, unknown host runs
  flat with a log line). Host detection is a separate pure function pair
  (`detect_host_game()`/`host_data_subdir()`) because framework init needs the per-game
  data subdir BEFORE `log::init` runs - so those functions must never log. The
  exe-name-to-game mapping lives only in the game layer; core stays exe-name-free.

- **2026-07-29 (session 24, M10): per-game data dirs - BS2 writes under
  `%LOCALAPPDATA%\BioshockVR\bs2\`, BS1's flat layout is FROZEN.** The alternative
  (subfolders for both, with a migration) was rejected: BS1 is released at v0.4.1, an
  external tester is mid-crash-investigation, and the calibration inis
  (vrpreset/hands/weapons) must not move or be clobberable by a BS2 session. Mechanism:
  `log::init(subdir)` appends the game-layer-supplied subfolder; empty keeps the BS1
  strings byte-identical. Two data-dir bypasses (command.txt in the BS1 poller,
  framedump in core's frame inspector) were rerouted through `log::data_dir()` so every
  future consumer inherits the subdir automatically. Rule going forward: NOTHING
  composes `%LOCALAPPDATA%\BioshockVR` by hand; everything asks `log::data_dir()`.

- **2026-07-29 (session 24, M10): the BS2 camera seam is a ProcessEvent hook filtered to
  the PlayerCalcView UFunction, learned via a FindFunctionChecked hook.** BS1's seam
  (hook the compiler-generated event thunk) is DEAD on BS2: the thunk exists and
  resolves, but the 16-minutes-earlier build inlined the event dispatch at every call
  site - zero static callers (offline caller census; full trace in
  docs/bioshock2/ENGINE_NOTES.md). Chosen shape: hook `UObject::FindFunctionChecked` to
  learn the PlayerCalcView `UFunction*` by comparing the FName index against the
  scan-derived cached-index global (zero UObject-layout assumptions), and hook the outer
  `UObject::ProcessEvent` (resolved through the controller vtable slot 3 stub chain,
  prologue-gated) to mutate the param block after the original returns. Rejected:
  hooking the dead thunk (unknown signature - a mismatched detour is a stack-corruption
  hazard); hooking individual inlined callers (35 sites, unknown signatures); naked
  counting probes (a diagnosis tool, not a seam). Costs accepted: every script event in
  the game crosses the detour (pre-filter work is two pointer compares), and the seam is
  one hop more indirect than BS1's. Benefit banked: the pair generalizes - any script
  event on any object is now hookable by name on BS2, which BS1 never got.

- **2026-07-29 (session 24, M10): core/adapter seam-leak inventory (the M10 acceptance
  criterion), and the policy: duplicate-in-b2r now, unify once BS2 stabilizes.**
  Bringing up the second adapter surfaced these leaks, each consciously NOT fixed this
  session to keep released BS1 code untouched (fixing them means refactoring v0.4.1
  paths in the same session that brings up a new game):
  1. The command-file seam lives in each ADAPTER, not core - a skeleton adapter has no
     commands until its first engine hook fires (BS2's poller now ticks from the
     ProcessEvent detour; BS1's from CalcView). The poll/parse/dispatch mechanics are
     duplicated, and core-owned commands (memscan family, dumpframe, vrinput, vrpace)
     are BS1-only vocabulary until the dispatcher moves to a shared home.
  2. The vrpreset/hands/weapons serializer persists CORE state (vr, input, hud) from
     BS1 adapter code - b2r deliberately writes no ini at all until this is resolved.
  3. The strict-gameplay predicate, 1 Hz heartbeat shape, and the whole 6DOF head-drive
     math (recenter latch, integer yaw residual, dual-frame position rotation) are
     duplicated in b2r/camera.cpp - candidates for game/shared once BS2's copy has
     survived in-headset use.
  4. `ue_math.h` moved to `game/shared/` (pure engine math, no addresses) with
     temporary `bvr::b1r`/`bvr::b2r` using-aliases so adapter code keeps unqualified
     spellings - a shared game-layer namespace convention is still owed.
  5. `IGameAdapter` remains the 4-method seam (capabilities/init/setFov/drawDebugUi);
     the documented 9-method target in this file's earlier sections is aspirational.
     BS2 confirms the pull-based reality works for a second game; the doc gap stays
     until the interface actually grows.
  Docs restructure that goes with all this: game-specific knowledge bases moved to
  per-game folders (docs/bioshock1/, docs/bioshock2/ - ENGINE_NOTES + TESTING each);
  project-wide docs (STATUS/ROADMAP/ARCHITECTURE/RESEARCH) stay at docs/ root.

- **2026-07-29 (session 24, M10): BS2 is not bound by BS1's methods - prefer the better
  native path when BS2 affords one (USER DIRECTIVE).** BS1's solutions are full of
  compensation machinery forced by BS1-specific limitations: the foreground/viewmodel
  lens split and its fovA/fovB calibration, weapon scale/placement compensation, the
  aim-seam workarounds, single-hand equip constraints. The user's explicit call: when
  BS2's build makes a cleaner method feasible, adopt it rather than porting BS1's
  workaround - BS2 already differs favorably (native FOV slider in the options UI,
  native dual-wield, ProcessEvent-by-name hooking from the inlined-dispatch discovery).
  Working rule for every subsystem brought to BS2: (1) check what BS2 does natively,
  (2) test whether the BS1 problem even EXISTS on BS2, (3) only then port compensation
  machinery, and only the parts proven necessary. First application: the FOV work must
  start by testing whether BS2's native FOV propagation already keeps the viewmodel
  correct across FOV values - if it does, BS1's entire fg apparatus stays unported.

- **2026-07-29 (session 25, M10): discovery tooling DUPLICATED into b2r per the
  duplicate-now policy - value-scan command routes verbatim, the vtable heap scanner
  with one deliberate divergence.** The memscan-family/dumpframe routes are ~70 lines of
  address-free forwarding copied into b2r's dispatcher; moving the dispatcher to a
  shared home was re-rejected because it would edit released bioshock1r/camera.cpp and
  add a shared TU for no behavioral gain (stays on the seam-leak list). The b2r copy of
  `scan_for_vtable_object` is BS1's shape plus a NEW `vtscan <hexRva>` probe command
  (one-shot candidate-vtable verifier, logs every live match), and the b2r settings
  locator bakes in 3-miss DORMANCY from day one - BS1's settings scanner predates the
  session-22 dormancy lesson and only rate-limits. Re-arm signal: view-state changes.

- **2026-07-29 (session 25, M10): BS2 FOV design - honest claim unconditionally, the
  write levers gated and default OFF, and NO foreground machinery.** Readback: every
  CalcView reads `UShockUserSettings+0x4C` into `vr::set_rendered_hfov`; a missing
  object claims 0, core's explicit "no readback" signal (falls back to the headset
  target - bit-identical to pre-readback behavior, so the bring-up path never
  regresses). Write: BS1's write-block shape (`vrfov` forced-headset wants strict
  gameplay AND an actively driving HMD; `gfov` manual wants strict gameplay; one-shot
  save/restore of the user's option), but the CalcView-silent stale-restore ticks from
  the ProcessEvent detour (gate: one bool read every 64th event) because BS2 has no
  scenedraw hook. Both levers DEFAULT OFF per the every-lever-off rule. The native-path
  check came FIRST per the s24 directive and returned the decisive verdict: poking the
  option re-lensed the drill viewmodel WITH the world (screenshots, ENGINE_NOTES) - so
  fovA/fovB/kFgEyeComp/vrfgfov stay unported, and BS2's FOV milestone is readback +
  write + nothing else.

- **2026-07-29 (session 26, M10): BS2 SequentialReentry runs on the THREADED
  substrate - none of BS1's single-threading machinery ports.** The policy gate
  ("check whether BS2 needs the BS1 workaround before porting it") ran against the
  derived substrate and returned no: BS1 forces the renderer inline because its game
  thread enters a racy kick-and-wait event handshake per frame (the 0x61D38E deadlock
  class); BS2's `UGameEngine::Draw` fills a cursor-based command ring with no
  handshake, and the per-tick render sync runs once AFTER the doubled call - so a
  second Draw just enqueues a second scene + present. Flat-proven: pulse + continuous
  + stereo all clean, `presents/s == 2 x draws/s` exact, zero faults. Consequences:
  no flush-point hook, no drain guard, no hw-thread quotient poke, no watchdog (the
  deadlock class it detects cannot form without the handshake; one gets added only if
  a hang is ever observed), and `vrstereo` sequences just camera mode -> stereo. The
  render-thread sync pair (FEventWin globals 0x1A69294/98) is banked in ENGINE_NOTES
  UNCONSUMED as the 1t-fallback derivation entry point if long soaks ever disprove
  this. Load safety replaces BS1's structural-1t discipline with a deny-by-default
  caller gate: pass 2 doubles only Draws whose return RVA is the single census-verified
  gameplay dispatcher (0xCD5D7B) plus the calcview-silent and present-stall skips.

- **2026-07-29 (session 26, HANG FIX, core - affects BOTH games): the unfocused
  pace KEEPALIVE is retired; an unbounded `xrWaitFrame` is never worth
  "insurance".** Field hang, found minutes after the BS2 stereo acceptance: the
  user took the headset off, the session dropped FOCUSED -> VISIBLE, and the game
  wedged solid (0.11 s CPU over 4 s, 1 running thread, `Not Responding`, kill
  required) with NO crash, NO dump and NO fault line - the log simply stopped one
  line after `xr: pace keepalive while VISIBLE`. Mechanism: the M8 stall guard
  skipped the blocking wait while unfocused BUT let one real paced frame through
  every 5 s as insurance, and `g_nextKeepaliveMs` starts at 0, so the FIRST
  unfocused present ran `xrWaitFrame` - which takes no timeout and, with the
  headset idle, never returned. On BS2 that call sits on the dedicated present
  thread, so it back-pressured the game thread through the render command ring
  and froze everything. The keepalive is now gone: once FOCUSED has been held,
  unfocused presents ALWAYS skip the wait. Recovery never depended on it -
  `pump_events` runs every present above the guard and the return to FOCUSED is a
  session event, not something an app earns by submitting frames. Worst case
  without it is "stays unfocused" (visible, recoverable, `vrpace off` restores the
  old behavior for A/B); worst case with it was a wedged process. Paired with a
  belt-and-braces guard: `on_present_begin` now closes any leaked open XR frame
  before waiting, since waiting on a begun-but-never-ended frame is the other way
  a runtime can block forever (the SR pair-hold is the only intended open-frame
  state, and it returns long before that point).

- **2026-07-29 (session 26, M10): BS2 pass-2 camera enters via the ProcessEvent seam
  replay, and commands can never execute mid-Draw.** The doubled Draw re-dispatches
  PlayerCalcView exactly once (live: 2nd-pass hits == second calls, 655/655), so the
  BS1 replay design transfers onto the ProcessEvent filter: the fn-match branch checks
  `second_pass_for_current_thread` and runs `second_pass_replay` (cached pass-1 base +
  +IPD/2, absolute writes = idempotent, no telemetry/drive/FOV/heartbeat side effects,
  and no `g_lastCalcViewMs` update so staleness keeps meaning "the NORMAL pass went
  silent") instead of `calcview_tail`. Hardening the BS1 shape improved on it: the
  command poller and FOV stale-restore now defer while `inside_hooked_call()` holds
  (BS1 polls from inside the hooked build; on BS2 a `vtscan` landing mid-Draw would
  freeze the pair and a hook toggle mid-Draw would be UB - the deferred tick lands
  milliseconds later given ProcessEvent traffic), and the overlay's vrstereo checkbox
  posts a request that the same outside-hooked-calls lane applies (also the only lane
  that works at BS2's menu, which never runs PlayerCalcView).

- **2026-07-30 (session 28): a measured value must carry the identity of WHAT it
  measured, not just its freshness.** The yaw warp was a live instrument reporting the
  foreground lens as the world lens. The value was fresh, structurally valid, correctly
  decoded, and labelled `src=live` at the point of use - and it was 1.84x wrong because
  it came from the wrong pass. Two habits caused it and both are now rules. (a) A
  cross-check must discriminate the thing that can actually be confused. The decode
  verified `2tanH` against `-tanH` and `-2tanV` against `tanV` - intra-axis, so it proved
  each triple self-consistent and carried zero information about which lens or axis it
  belonged to. The three structural zero slots that DO disambiguate were in the offline
  decoder from day one and never ported. (b) A single sample is an assumption about
  homogeneity. The frame is not homogeneous: it holds two lenses that agree only at
  16:9, so "the first decodable draw" silently encoded "whichever pass draws first".
  Sampling is now strided across the whole pass and voted, with the runner-up published
  as a named second lens - the ambiguity is a reported quantity instead of an invisible
  coin flip - and a round that lacks a clear majority or that failed to span its pass is
  REFUSED rather than published, on the principle that holding a stale value the age gate
  will expire beats publishing a confident wrong one. Corollary for consumers: a source
  tag (`src=live`) says where a number came from, never that it is right, and the
  session-27 elimination of the FOV hypothesis rested on exactly that confusion.

- **2026-07-30 (session 28): an instrument that cannot fail its own hypothesis is not
  evidence.** `fovaudit pose on` was built to check that the submitted layer pose matches
  the pose the frame was rendered from, and it compares `projViews[0].pose.orientation`
  against `g_consumedHeadQuat` - both stamped from the same `xrLocateViews` generation.
  `delta 0.00 deg` through real head sweeps was therefore guaranteed by construction, and
  it was written up as having eliminated the pose hypothesis. Rule: before trusting a null
  result, name the two sources and check they are independent. Where the render-side truth
  already exists (here: `g_eyeCamRot[eye]`, the rotator the drive actually wrote, stashed
  per eye per tick), the audit must compare against THAT.

- **2026-07-30 (session 28): a bounded promise must be enforced with a deadline, and a
  latch must never sit above the event pump.** The SR pair-hold leaves an XR frame open
  across two presents on the promise that the sibling present arrives within one build
  (~1-4 ms measured). Its early return sat ABOVE `pump_events()`, so for as long as the
  promise went unkept nothing polled XR events: session state froze, every guard below
  became unreachable, and the only escape left in the whole function was the VR-disable
  teardown. Alt-tab stops presenting mid-pair, so the promise went unkept indefinitely.
  Two structural rules now: state that gates an early return is refreshed BEFORE that
  return, and any "the next call will finish this" state carries a timestamp and a
  force-abort path (500 ms here, ~125x the measured need). Also: recovery diagnostics must
  not be gated on the same variables the failure corrupts - `pacing SKIPPED` required
  `g_unfocusedSinceMs == 0` and `FOCUSED again` required it non-zero AND `g_everFocused`,
  which STOPPING cleared, so after one stop the log went silent in both directions and a
  stuck user's log read as "nothing happened".

- **2026-07-30 (session 28): the pair-hold rules above stand, but the pair-hold was NOT
  the alt-tab cause - and shipping it as one is the lesson.** The entry above reasoned from
  code structure to a plausible mechanism and fixed it. The instrument shipped alongside it
  then measured the real thing: `SUBMISSION IDLE (reason=pace guard: session not FOCUSED |
  state=VISIBLE everFocused=1 pairOpen=0 ...)` repeating with no FOCUSED line until the VR
  toggle. `pairOpen=0` on every line - the hold was never set. The structural fixes remain
  correct and worth having; the attribution was wrong, and it was wrong for the same reason
  the FOV hypothesis was wrong two commits earlier: a mechanism that explains the symptom is
  not evidence that it caused it. Rule: when a fix is derived from reading code rather than
  from a measurement, say so in the commit and ship the instrument that can refute it in the
  same build. That is what made the third attempt at this bug class the one that worked.

- **2026-07-30 (session 28): a guard must key on the hazard it exists to avoid, not on a
  proxy for it - and an unbounded call belongs off the thread that must not block.** The M8
  pace guard existed to dodge a blocking `xrWaitFrame`, but it keyed on SESSION STATE: any
  present while not FOCUSED submitted nothing. Measured cost: 5772 presents skipped with
  ZERO `xrWaitFrame blocked` lines in the whole session - not one slow wait to justify any of
  it. Worse, it was self-sustaining: VDXR will not re-grant FOCUSED to an app that submits
  nothing, so the guard's own effect kept its own precondition true. A circular wait, and the
  reason the alt-tab freeze was permanent while flat rendering continued. It also falsifies
  the session-26 note that recovery "is not something an app earns by submitting frames" - on
  this runtime it is earned. The fix is structural rather than a better predicate:
  `xrWaitFrame` takes no timeout and never can, so it now runs on a dedicated pace thread and
  the present thread waits on the result with a deadline (200 ms FOCUSED so the headset still
  paces the game, 20 ms otherwise). An unbounded block can no longer reach the thread the
  game depends on, which retires the session-26 hang class permanently instead of trading it
  for the freeze. Consequence to respect: a session must never be destroyed while that thread
  is inside `xrWaitFrame` on it, so `teardown_session` defers and retries rather than handing
  the runtime a freed handle - "stays up but idle" is an acceptable worst case, a
  use-after-free is not.

- **2026-07-30 (session 30): a size coincidence is not a discriminator, and at a square render
  target the post-FX rule stopped being one.** Session 22 routed a post-tonemap draw in-frame
  when `srv0 dims == target dims`, on the stated premise that post effects sample a
  BACKBUFFER-SIZED texture while gameswf samples 2048x2048 UI atlases. The user runs 2048x2048,
  where the backbuffer IS 2048x2048 and the premise inverts: measured, `postFxRejected=1604161`
  against `postFx=2` genuine, i.e. about 30 gameswf HUD draws per interval taking the in-frame
  exit. The rule is now structural - a post-FX source is something the engine RENDERED
  (`BIND_RENDER_TARGET`), a UI atlas never is - which is stronger at every resolution rather
  than only at square ones, and `vrcine postfx size|rt` keeps the old rule for a one-command
  A/B. The general form worth carrying: a discriminator built from a numeric coincidence
  between two quantities silently dies the day the two quantities coincide for another reason,
  and it dies without an error. Prefer a property that is true BY CONSTRUCTION of the thing
  being discriminated.

- **2026-07-30 (session 30): `PassThrough` was a verdict with no owner, so it is now measured
  rather than assumed.** The redirect binds our RTV through the ORIGINAL `OMSetRenderTargets`,
  so `hud::on_setrt` never sees it and `g_curRt` keeps naming the game's target. The classifier
  could therefore return "in frame" while the device had the HUD capture RT bound, and which
  draws that happened to depends on batch ORDER, not on the classifier. `hud_capture.h` had
  stated the contract in prose since session 19; session 30 turned it into per-reason pass and
  STRANDED counters plus a one-shot `OMGetRenderTargets` check that proves the belief against
  the device. The measurement then REFUTED the hypothesis it was built for - effect fills read
  `effect=127010/0`, never stranded - while a positive control (`vrcine postfx size`) made the
  same counter read 36140, proving it can fire. That combination, self-check plus positive
  control, is what makes a negative result trustworthy; a zero from an instrument that has
  never been seen to fire is not evidence of anything.

- **2026-07-30 (session 30): a residual classification rule has no upper bound, and one had
  already shipped.** The in-frame effect test was "textureless AND not the bar vertex count",
  which is the complement of two things rather than a description of one. The live census had
  already recorded a 1493-vertex textureless draw - a tessellated vector shape, not a
  full-screen fill - and the residual rule was rendering it into the eye image. It is now a
  positive bound (`vertexCount <= 8`, retunable via `vrcine effects verts <n>`), and anything
  outside it goes back to the panel. Same shape as the session-29 bar fingerprint decision: the
  vertex count is a named, live-retunable constant and every other count still logs once, so a
  wrong bound shows up as data instead of as a silent miss.

- **2026-07-30 (session 30): measuring first killed the fix we were about to build.** The
  release-blocking wrench bug had a well-argued leading hypothesis - the wrench is an `AWeapon`,
  our origin substitution moves its fire start, melee is a short trace - and a designed fix
  behind two levers. Measured in-headset with the wrench equipped: the weapon seam took ZERO
  calls and every ability-seam call was a plasmid. Melee reaches neither seam, so `vraim seam
  weapon off`, `vraim origin off` and a melee carve-out in `substitute()` would all have done
  exactly nothing. The measurement cost one read-only console command and one play session; the
  fix would have cost a build, an in-headset round, and a false belief that the bug was closed.
  The corollary that made it cheap: `vraim probe on` + `vraim off` installs the hooks with
  substitution refused, so the diagnostic run cannot change the behaviour it is measuring. Any
  future "is this seam involved at all" question should start there. Note the one trap - VR
  PRESET 1 re-arms `aim on` and `origin on`, so read-only mode has to be re-armed after a preset
  press.
