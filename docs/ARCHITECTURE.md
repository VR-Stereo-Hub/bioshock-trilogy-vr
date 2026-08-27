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
    virtual bool handleCommand(const char*, const char*) { return false; } // command seam (s35)
};
```

Rules that keep the seam clean:

- **No raw addresses outside `game/<title>/patterns.cpp`.** Every resolved address flows through
  a named symbol table, logged at startup and mirrored in ENGINE_NOTES.md.
- Core never touches UObject/FName - core speaks poses, meters, and D3D11. Adapters own all
  engine semantics, including units (meters ↔ Unreal units, FRotator 65536/turn ↔ radians;
  `worldScale` config-overridable).
- **Adapter -> core state travels as SELF-EXPIRING PUBLISHES, never as a query.** Core cannot call
  into the adapter mid-frame (wrong thread, wrong lifetime, and it would invert the dependency), so
  the adapter pushes a bool or a float from its CalcView tail and core reads a timestamped slot:
  `publish_vr_gameplay` (the stick-pitch-kill gate), `publish_pitch_error` (the pitch servo),
  `swing::publish_gate` (is the wrench equipped). All three share one discipline - a 500 ms
  staleness budget, and the stale state is the SAFE one, so a publisher that stops disarms the
  feature rather than freezing it on its last value. New cross-layer state should copy this shape
  rather than inventing a fourth.

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
### Motion gestures: the pattern (session 31, `core/input/swing.{h,cpp}`)

Swing-to-attack is the first gesture that turns physical motion into a game input, and it is
built as a reusable shape rather than a one-off. Anything else of that kind - a shove, a
throw, a reload flick, a two-handed brace - should follow it.

**Layering.** The detector is CORE (`core/input/`), not the XR layer, and not the adapter:

| piece | where | why there |
|---|---|---|
| pose sample in, once per XR frame | `core/vr/openxr_input.cpp`, one call | it is the only place that has the frame's poses and its predicted display time |
| threshold / hysteresis / cooldown / pulse | `core/input/swing.cpp` | the only layer that runs with AND without a headset, so the whole decision path is testable flat |
| "would this input be legitimate right now" | the game adapter, published per CalcView | only the adapter knows what is equipped and what view is up |
| the synthetic input itself | `core/input/xinput_bridge.cpp`, one line in `compose_synthetic` | gestures are just another producer over the same merge |

The temptation is to put the detector where its data is born, in the XR layer. Do not: that
file compiles only under `BVR_WITH_OPENXR` and its per-frame entry point never runs without a
headset, so a detector there cannot be tested at all until someone puts a headset on.

**Four rules the first one established.**

1. **Read poses through the funnel** (`input_get_hand_pose`), never off the slot behind it. The
   funnel is what the session-20 recorder's sim overlay injects onto, so a replayed session
   drives the gesture exactly as it drives the ray, the viewmodel and the laser. Runtime
   velocity (`XrSpaceVelocity`) would be marginally more accurate and invisible to every replay
   tool we own; finite-differencing the funnel poses is worth more than the accuracy.
2. **Gate on IDENTITY, not plausibility.** A synthetic input means whatever the game currently
   binds it to - the swing composes RT, and RT with a gun in hand is a *shot*. The gate is
   therefore "the equipped holdable's class name is X", reusing an identity the mod already
   maintains, never a heuristic like "that motion looked like melee". Every gate fails CLOSED
   and expires on a staleness budget, so a publisher that stops (world unload, drive off)
   disarms the gesture rather than latching it open.
3. **Hysteresis AND a cooldown, because they catch different things.** One motion accelerates
   and decelerates through the threshold, so without a re-arm latch a single gesture fires
   twice; the cooldown bounds a shake. Report one verdict per GESTURE, not per sample - the
   first build logged 106 identical "blocked" lines for one simulated swing.
4. **Ship a `sim` command that drives the real decision core**, and give it a repetition count.
   One synthetic gesture crosses the threshold once, and the command seam polls at 1 Hz, so no
   pair of commands can ever land inside a sub-second cooldown: without repetitions the
   cooldown is untestable flat. With it, every threshold, gate, latch and cooldown was verified
   before the feature ever reached a headset, and only the tuning constants needed a human.

**Tuning constants are the only thing a headset is required for.** Ship them as named
constants, expose them as commands + persisted preset keys + overlay sliders, and have `status`
report the PEAK measured value since the last call - that is the number that replaces the
guess. Swing-to-attack shipped to its first headset run at 2.2 m/s and came back at 3.6.

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

- **2026-08-06 (session 43) · Infinite stutter hunt: the spike instrument attributes at the
  PAIR CLOSE, samples MID-STALL, and the fix gate is evidence-first (user directive).** The
  1 Hz aggregates could say a second was bad, never which phase carried it; the s43 design
  answers in two layers that both ship opt-in (default off in core, armed with Infinite
  stereo, `vrpace spike` seam): (1) at the pair close - the one per-pair single-thread
  moment - an over-threshold interval snapshots the per-phase last/max tables (maxima reset
  per spike, so bursts self-scope) plus the UNATTRIBUTED remainder, which is the
  ours-vs-game discriminator; (2) a 4 ms sampler catches the still-stalled draw thread and
  stack-captures it through the s34 watchdog, because a stall our phases do not contain can
  only be named by its own call stack. The user's standing gate, recorded mid-session: no
  fix attempts until the cause is GUARANTEED by evidence - honored by naming the 30 s GC
  tick through an A-B-A ini intervention (the DefaultEngine.ini propagation lane was proven
  as part of the same probe) before the interval change was left in place as the headset
  candidate. The alternative - trying the community's ranked levers directly - was
  declined; a lever that moves the spike rate without a named mechanism would have been
  indistinguishable from path variance.
- **2026-08-05 (session 42) · Infinite I6/I7: the pair-rate sync gates at PAIR-OPEN only, and
  it ships default-off in core, default-on with Infinite stereo.** The judder investigation
  measured that a strictly-gating xrWaitFrame (the sim's) locks the pair rate to refresh with
  the present thread parked in the wait handoff - so the s41 "free-run beat" suspect is a
  property of a PIPELINING runtime, decidable only by the new TRACE pairs telemetry under
  VDXR. The sync (`set_pace_sync` / `vrpace sync`) therefore exists as an armable A/B rather
  than an unconditional fix: it delays only the present that OPENS a pair (delaying the
  closing RIGHT present would stretch the 1-4 ms intra-pair gap pair pacing exists to bound),
  targets the runtime's own predictedDisplayPeriod (published for the first time; commanded
  Hz as the fallback), and self-collapses to no-delay when the game is slower than the
  schedule - measured near-inert against a gating runtime (sd tightened 1.2 -> 1.0 ms, gate
  moved from the wait to our side). Default OFF in core with zero new branches taken for
  BS1/BS2 (the set_pace_detach pattern; BS1 full-sim-lane proof in the commit); the Infinite
  adapter arms it inside `apply_vrstereo(true)` and disarms on the symmetric off - so the
  headset A/B is one F10 checkbox.
- **2026-08-05 (session 42) · Infinite I7: the loadout/cheat lane dispatches ProcessEvent on
  the OWNING OBJECT; the console script-exec bridge is a recorded structural negative.**
  Measured in a gameplay save: every script-side exec through ConsoleCommand is inert (god,
  AllWeapons, behindview, viewmode - pixel-identical screenshots) while C++ handlers stay
  proven; the give-family names do not exist in a full GNames dump; XCheatManager is never
  instantiated (the PC carries only CheatClass at +0x344), so the entire CheatManager
  vocabulary is structurally unreachable - not gated, absent. The design consequence: the
  reflect lane grew `bsifields` (walk the latched PC's pointer fields, identify UObjects by
  class name via the live-derived UObject::Name +0x18, hook-parameter objects only - never a
  scan) and `bsicallat` (the bsicall gate stack against an explicit object), and grants go by
  ProcessEvent on the pawn (AcquireWeapon wants a weapon object - the s43 seam). The
  alternative - resurrecting the console bridge or constructing a CheatManager - was
  declined: both mean building engine machinery the shipped game deliberately does not run.
- **2026-08-05 (session 41) · Infinite I6: the FOV lever ENFORCES per dispatch, and the claim
  derives from the lever through the 16:9-referenced law.** The named-property lane was tried
  first per the roadmap and is measurably dead (`set XUserOptionsManager FieldOfView` writes
  nothing - zero stable holders after a scan for the written value), and every FOV cache in the
  process (six 82.5f holders) snaps back within a tick of a poke: the engine recomputes the
  chain from the option each tick, so no single write can be the lever. The lever therefore
  writes the camera object's two FOV fields (`[cam+0x214]`, `[cam+0x3D0]`) on every
  GetPlayerViewPoint dispatch - the same seam BS2's option-write lever uses, with the engine's
  own recompute as the disarm restore. The claim wiring was then CORRECTED BY ITS OWN AUDIT:
  the degrees value is horizontal at a FIXED 16:9 reference (tanV = tan(deg/2)/(16/9), pinned;
  tanH = tanV x actual aspect), not at the current aspect - the lens decoder flagged the
  current-aspect claim 43.7% wrong on its first 1:1 round, both decoders agreed on
  tan(50)/1.7778 = 0.6704 exactly, and patterns.h now carries `kFovRefAspect` with the
  measurement. The rejected alternative - publishing the decoder's output as the claim
  unconditionally - was declined: the commanded-value-through-the-law claim is exact by
  construction while the lever is armed, and the decoder stays the independent AUDIT (with
  `bsilens track on` as the opt-in coupling for lever-off states).
- **2026-08-05 (session 41) · Infinite I6: the config registry stays ADAPTER-LOCAL; the
  ROADMAP's "extract bvr::config into core" is deferred to the healing session.** Infinite is
  the third consumer of preset persistence, which is the extraction trigger the roadmap named -
  but the decoupling directive outranks it: BS1/BS2 are headset-accepted, their hand-rolled
  vrpreset writers round-trip core-owned state through adapter files in ways a shared registry
  would have to reproduce exactly, and a core module whose only consumer is Infinite buys
  nothing this session that a 300-line adapter file does not. `bioshockinf/config.cpp` owns the
  KeyDesc table, vrpreset.ini and named presets; the ROADMAP line is annotated rather than
  silently dropped. A second decision rides along: a loaded preset's RESOLUTION is latched into
  the picker and never auto-applied - a surprise live resize mid-headset (backbuffer +
  XR-swapchain rebuild) is a session hazard, so the one overlay-clickable Apply stays the only
  resize path.
- **2026-08-05 (session 40) · Infinite I5: the SR root must INCLUDE the present kick, and the
  FOV claim is a constant-tanV law until I6.** Two decisions. (1) The doubling root is the
  viewport draw (`0x1FDE30`, canvas -> client draw -> present kick), not the client draw
  (`0x26A3E0`) whose body actually holds the camera loop - doubling the client draw was tried
  first and produced camera+scene doubling with NO second present (the present is kicked by the
  caller's tail), which starves core's one-eye-per-present model and skews the SR tag ring +1
  per tick. The general rule, now measured on a second engine family: the SR root is the
  smallest function whose call tree contains camera sample, scene build AND present. (2) The
  projection claim is computed from the I2 law with a CONSTANT tanV (slider-min 0.4317,
  `bsifov tanv` lever, vrpreset-persisted) rather than a live option read - Infinite's option
  pointer is not derived yet and I6 owns that lever. The cost is honest and documented: a user
  who moves the in-game FOV slider makes the claim stale until corrected. The alternative -
  pulling I6's decoder forward - was declined per the restructured ladder (only a lens question
  the law cannot answer justifies that).
- **2026-08-05 (session 39) · Infinite I4: MonoTracked runs on the QUAD, and the drive writes
  out-params only.** Core couples `set_camera_mode(true)` to flipping submission from the quad
  to a projection layer ("never let a head-driven camera show on the quad" - a BS1/BS2
  convention). Infinite's ladder deliberately breaks that for one rung: the I4 drive gates
  adapter-locally (`bsicam drive` + `get_head_pose`) and never touches camera mode, so the
  head-driven camera shows on the mono quad until I5 earns the projection flip with the FOV
  law. No core change was needed - the coupling stays intact for BS1/BS2. Second half of the
  decision: the drive substitutes the GetPlayerViewPoint OUT-PARAMS and never writes
  `[cam+0x3B8]`/engine memory - drive-off is a byte-identical passthrough, the engine's view
  stays engine-owned (heartbeat prints engineRot + pitchErr as the read-back), and the BS1
  pitch-freeze class of bug is impossible by construction. The pitch servo
  (publish_pitch_error/publish_vr_gameplay) is deliberately NOT armed: with the bridge live it
  seizes right-stick Y, which is I7's lane to own.
- **2026-07-31 (session 36) · The game-thread command pump holds a LEASE, not an eviction.**
  Session 35 made `poll_from_game_thread` silence the Present pump permanently, on the reasoning
  that a resume-on-stall rule would hand the render thread a dispatch exactly during a load, "the
  worst moment available". That hazard is real, but it was stated too broadly: what must not happen
  during a load is an *engine-touching* dispatch, not any dispatch at all. As written, an adapter
  whose hook went quiet (level load, Scaleform menu, scripted camera) lost its command surface
  entirely, with no log line saying so and no way back. The pump now resumes after a 3 s lease in a
  **degraded** mode that refuses `mempoke*`/`pokeaddr*`/`memrestore` and dispatches everything else,
  and the game thread reclaims it on its next call. This also made the handover testable on demand,
  which the original was not. Inert for BS1/BS2 by construction: neither includes
  `core/framework/command.h`.
- **2026-07-31 (session 36) · `frame_inspector` gains a third arm mode rather than widening mode 2.**
  Mode 2 reads back the bound VS b0 once per distinct *buffer object*, which is correct for a
  Map/WRITE_DISCARD engine that renames its constant buffer on every upload. UE3 titles need not
  behave that way - BioShock Infinite reuses a handful of objects and rewrites them with
  `UpdateSubresource` - so mode 2 both under-samples and misattributes there. Mode 3 (`dumpframe
  cb`) captures the payload from the `UpdateSubresource` call parameter instead: no staging buffer,
  no `Map` stall, no readback race, every stage and slot, at the buffer's real size. Kept strictly
  additive (new mode word, appended dump sections, event-line tail after `stk=`) because BS2 is
  developed in these files in parallel; the additivity is *tested* by running the previous decoder
  against a mode-3 dump, not asserted.
- **2026-07-31 (session 36) · Core's `decode_ray_block` shape does not generalise past Vengeance,
  and the replacement is offline-first.** The `(2tanH, 0, -tanH, 0, 0, -2tanV, tanV)` helper is a
  BioShock 1 and 2 fact. UE3 hands the shader a 4x4, so `decode-framedump.ps1 -ScanMatrix` recovers
  the tangents from column norms (`tanH = |c3|/|c0|`), where the object scale cancels and a
  per-object constant buffer is therefore usable. A live equivalent inside `hud_capture` is
  deliberately NOT built yet: it would be core machinery with no consumer until the Infinite offset
  is actually known. Revisit at I5.

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

- **2026-07-31 (session 31): a gesture detector belongs where it can be TESTED, not where its
  data is born.** The wrench swing detector's inputs (hand poses, per-frame timing) all live in
  `core/vr/openxr_input.cpp`, which is the obvious home - and the wrong one: that file compiles
  only under `BVR_WITH_OPENXR` and `input_sync` never runs without a headset, so a detector there
  could not be exercised flat at all. It lives in `core/input/swing.{h,cpp}` instead, next to the
  bridge that owns the composed pad, with the XR layer reduced to "here is a sample". The whole
  decision core is then reachable from a `sim` command that runs on the game's own XInput poll,
  and every threshold, gate, latch and cooldown was verified flat before the feature ever reached
  a headset. The XR layer keeps exactly one line of the feature.
  *Corollary that shaped the test seam: `sim` takes a REPETITION COUNT.* One synthetic swing
  crosses the threshold once, and the command seam polls at 1 Hz, so no pair of commands can ever
  land inside a 300 ms cooldown - the cooldown was untestable until the sim itself could produce
  two swings 400 ms apart.

- **2026-07-31 (session 31): read the poses through the funnel, not the slot behind it.** The
  detector could have differenced `g_hands[1]` directly, or asked the runtime for
  `XrSpaceVelocity`. It reads `input_get_hand_pose()` instead - the same accessor the fire ray,
  the viewmodel and the laser use - so the session-20 recorder's sim overlay drives the gesture
  exactly as it drives everything else and a replayed swing is one consistent world. Runtime
  velocity would have been marginally more accurate and invisible to every replay tool we own.

- **2026-07-31 (session 31): a synthetic input needs an identity gate, not a plausibility gate.**
  A swing composes a full right trigger, and RT with a gun in hand is a SHOT. The gate is
  therefore the equipped holdable's class name (`aim::weapon_key_is("Wrench")`, reusing the
  per-weapon profile key rather than resolving the holdable a second time), never a heuristic
  like "the hand is moving like melee". Everything else about the gesture is a comfort tuning
  knob; that one line is the safety property, and it is the first thing the flat checklist tests.
- **2026-07-31 Â· session 32 Â· BS1's square-backbuffer policy does NOT apply to BS2, and the
  resolution lane is per-game in FILE as well as in shape.** BS1's settled policy (set the ASPECT
  with the resolution lane, leave the FOV option alone, because a square backbuffer fills a
  roughly-square headset eye) is now explicitly scoped to BS1. Measured on BS2: a 2048x2048
  backbuffer renders the scene into a 2048x1421 viewport with a black band, collapses the world
  horizontal from 100 to 67.7 deg, and produces a ray block whose two vertical encodings disagree -
  i.e. the projection stops being a consistent perspective off 16:9. On BS2 the resolution lane is
  therefore a SHARPNESS lever at 16:9, not an aspect-matching one, until a bisection finds which
  aspects (if any) BS2 renders full-height. Also per-game: BS2's viewport lives in `Shared.ini`
  `[SharedOptions] ViewportX/Y`, and the `[WinDrv.WindowsClient]` keys BS1's whole lane writes are
  IGNORED by BS2 (they are an output the game rewrites from its live state). `game_ini` is
  duplicated per adapter rather than shared, following the recorded duplicate-now seam policy - a
  third consumer should trigger the extraction, and the generic part (section-scoped ini editing
  with backup + temp + ReplaceFileW + read-back) is what would move.
- **2026-07-31 Â· session 32 Â· A verified write is not an honoured one; acceptance is the
  observable effect.** The BS1-shaped BS2 resolution port wrote its keys, re-read them, and logged
  "verified" while the engine rendered a different resolution entirely. Read-back proves the FILE
  took the value and nothing more. This is the same failure class as `-> HANDLED` from the Exec
  seam (session 30) and it now has two instances, so treat it as a standing rule: for any write
  that leaves our address space, the acceptance criterion must be a measured downstream EFFECT (the
  backbuffer at first Present, the ammo counter, the visible behaviour), never the write's own
  confirmation.
- **2026-07-31 Â· session 32 Â· cb0 layout offsets are PER-GAME constants, published by the adapter,
  with a self-correcting hunt as backstop.** The screen-ray helper's float index was hardcoded to
  BS1's 12 in both core (`hud_capture.cpp`) and the offline decoder. BS2's is 16 - same shape, four
  floats later - so the live watch decoded nothing on BS2 and the offline decoder decoded zero
  blocks. The offset is now `bvr::hud::set_ray_block_offset()`, published from
  `bioshock2r/patterns.h` at adapter init, keeping engine constants in the per-game patterns header
  as the project rule requires. Core also widened its cb0 copy window from 80 to 1344 bytes (a
  block past float 19 was previously not even COPIED, so no offset could have rescued it) with
  per-slot length tracking, and gained a hunt that only runs after the configured offset fails
  repeatedly, adopting and LOGGING what it finds so a wrong constant names its own correction. BS1
  is unaffected by construction (offset 12 validates on the first sample, so the hunt never runs)
  and was regression-checked: WORLD tanH=1.191754 at 2048x2048, bit-identical to the banked
  session-28 value.
- **2026-07-31 Â· session 32 Â· BS2's two lenses differ in MAGNITUDE at every aspect, unlike BS1's.**
  BS1's split was two aspect CONVENTIONS that coincide exactly at 16:9, so its symptoms were
  aspect-gated. BS2 carries a world lens that follows the FOV option and a second lens fixed at
  tan(30) = 60 deg that ignores it, distinguished by callstack, and they differ at 1920x1080. Since
  one projection layer carries one fov claim, that is a 2.06x angular-gain error on whichever layer
  the claim does not match, at 16:9, growing to 3.99x at option 130 - which matches the reported
  stereo viewmodel symptoms (wrong depth, moves with the head) far better than BS1's mechanism,
  which could not produce a symptom at 16:9 at all. Whether that 60-deg cluster IS the viewmodel is
  NOT yet proven and must be settled by making it move (holster/switch the weapon and re-dump),
  never by draw counts - BS1's rule, which is in the notes because inferring it from counts is what
  went wrong there.
- **2026-07-31 - session 35 - the command-file poller belongs to CORE and ticks from Present, not
  from an engine hook.** On BS1 and BS2 the poller lives in the adapter and ticks off the camera
  hook, so a skeleton adapter has no command surface at all until its first engine hook fires: the
  only way to talk to the mod is the thing that is not working yet. Both games paid for that in
  their early sessions. `core/framework/command` now owns the poller and `d3d11_hook`'s Present
  detour ticks it, which is why the Infinite adapter could be driven from frame one with
  `capabilities() == 0` and nothing hooked. The Present pump is **opt-in**
  (`command::enable_present_pump()`, called by the adapter) precisely so an adapter that polls for
  itself can never end up with two pollers racing over one file - BS1 and BS2 are byte-untouched
  and see one atomic load per present. Handover is **one-way**: the first
  `poll_from_game_thread()` silences the Present pump for the life of the process, because
  engine-touching commands belong on the game thread and a "resume when the game thread goes quiet"
  rule would hand the render thread a dispatch during a load, which is the worst possible moment.
  The trade, stated plainly: while the Present pump owns the poller, every command runs on the
  render thread, so a long `memscan` stalls presents instead of the game thread.
- **2026-07-31 - session 35 - `IGameAdapter::handleCommand` is a control-plane call, and that is
  not a violation of publish-don't-query.** The rule above ("core cannot call into the adapter
  mid-frame") is about per-frame STATE: wrong thread, wrong lifetime, inverted dependency. A
  command dispatch is once a second, on whichever thread owns the poller, and carries no engine
  state either way - the same shape as the overlay calling `drawDebugUi()` on the render thread.
  Dispatch order is adapter first, then the shared core vocabulary, so a game can deliberately
  shadow a core command; the virtual is defaulted to `return false` so adapters that own their own
  dispatcher need no change. The ~70 lines of core-owned vocabulary that BS1 and BS2 each forward
  by hand now exist in core as the canonical copy; **folding the two adapters into it is deliberately
  deferred** to a consolidation pass, because a parallel BS2 session was live in the same file and
  neither shipped game could be smoke-tested from the Infinite branch.
- **2026-07-31 - session 35 - a pre-existing `command.txt` is skipped at startup, not executed.**
  The poller's first poll adopts the file's write time and logs what it ignored. A command file
  left over from a previous run is stale by definition, and applying it at boot is a trap the
  testing notes record as having bitten BS1 three times, once producing a false result that was
  then chased as a real one. The subtlety, found by running it: prime on the FIRST POLL, not on the
  first sighting of a file - a game that starts with no `command.txt` (the documented hygiene) would
  otherwise swallow the first real command, which is exactly what the first build did.

### 2026-07-31 (session 33) - BS2 viewmodel lens, and what an instrument may be trusted to say

- **The FOV LAW is per-game, like every address.** BS2's option is a 16:9-referenced horizontal
  (`tanV = tan(opt/2)*9/16`, aspect-invariant; `tanH = tanV * bbW/bbH`); BS1's is a true
  horizontal. Opposite conventions, same engine tree. Third instance of never-copy after the ini
  keys and the cb0 offset - and the first where the thing copied would have been a FORMULA rather
  than a number. Adapters publish their own law; core holds none.
- **A lens match is written as a LIVE FOV-DEGREES EQUALITY, never a measured tangent.** The value
  written is derived from the option read THIS frame. BS2 also animates its own FOV (73 -> 96 deg
  during play), so anything that caches the option is wrong here regardless.
- **`lenses == 1` is not an acceptance criterion, and a sampled instrument must say so.** The fov
  watch sees ~12 of 400-600 constant buffers; the foreground pass is ~17 of them. Absence of a
  second lens is the ordinary case, not evidence of a match - reading it as evidence produced six
  false positives in a row. The header now states what the number is worth, and acceptance is a
  frame dump, which sees every block. **Generally: an instrument that reports the SUCCESS state
  when it fails to measure is worse than no instrument**, so where a cheap check cannot be made
  sound, say what it is worth rather than making it look sound.
- **Two fixes to that sampler were tried and both are recorded as dead ends in the code**: a
  head-slot reservation (the fg pass moves between captures) and a rotating stride phase (correct,
  and 20x the frame time - copying from a different set of dynamic buffers each interval defeats
  the driver's fast path). Coverage was the wrong goal; honesty about coverage was the right one.
- **A diagnostic on the present path gets a hard rate limit, not just a change test.** The
  fov-watch log line's change test could flicker once sampling became intermittent, so it fired at
  present rate, took the game to 40 fps and then wedged it. Change tests are policy and policies
  get edited; the floor is the safety net.
- **VR ENABLE/DISABLE MUST BE SYMMETRIC.** `vrcam on` enabled VR, `vrcam off` only cleared the
  camera mode - so once an OpenXR session was running, nothing in the command surface could stop
  the game being paced by it. With the session not FOCUSED the runtime paces its not-visible
  cadence (~10 Hz) and the game inherits it, which reads as a hang. Every toggle that starts a
  subsystem must be able to stop it.
- **"Not blocked" is not "not harmed" (correction to session 28).** Moving xrWaitFrame off the
  present thread stopped an unbounded wait from wedging the game, and that holds. It did not stop
  the frame HANDOFF from pacing the game thread to the runtime's cadence. Open.
- **Anything the user must judge by eye belongs in the F10 overlay, not in a seam command.** The
  overlay renders into the backbuffer, which IS the eye image. Driving an in-headset A/B by typing
  requires alt-tab, and alt-tab is the pacing bug - so command-driven headset testing is both
  slower and actively destabilising. Build the control before asking for the test.

### 2026-08-01 (session 34) - a simulated Quest 3, so agents can test their own VR work

- **Why a purpose-built runtime and not an off-the-shelf simulator.** BioShock is 32-bit, and
  essentially nothing ships a 32-bit OpenXR runtime: Meta XR Simulator is x64 (built for Unity
  and Unreal editors), OpenXR-Simulator is x64 and keyboard-driven, `ox` has no D3D11. An OpenXR
  API LAYER cannot substitute either - a layer needs a working runtime underneath, and with no
  headset VDXR returns FORM_FACTOR_UNAVAILABLE, so there is no session to intercept. Writing one
  was tractable because the mod's whole OpenXR surface is 39 entry points, one extension
  (XR_KHR_D3D11_enable) and one interaction profile.
- **Selected per-process by XR_RUNTIME_JSON, never by the registry.** The loader checks that env
  var before the registry (verified in the vendored SDK, manifest_file.cpp), so
  `tools\xrsim-launch.ps1` sets it, starts BioshockHD.exe DIRECTLY, and restores it in a finally.
  The machine's ActiveRuntime stays on VDXR, so the two coexist with zero switching and a normal
  launch is untouched. Cost: the game cannot be started through Steam in sim mode, because Steam
  launches the process itself and the variable would have to be machine-wide.
  **Zero lines of the mod changed** - `bioshockvr.dll` and the proxy are byte-identical, and the
  sim is a separate target that links nothing from them.
- **Two silent-failure traps, both guarded by a thrown error rather than a warning.** An elevated
  shell makes the loader ignore XR_RUNTIME_JSON (it reads it through a secure-env path), and a
  bad manifest path or 64-bit dll makes it skip the manifest. Both fall back to VDXR *silently*,
  so every later measurement would be taken against the wrong runtime while the transcript said
  otherwise. `xrsim-launch.ps1` asserts the runtime NAME out of the mod's own log and throws on
  anything that is not `bvr-xrsim`; `xrsim-install.ps1` checks the dll's PE machine is 0x014C.
  This is the same lesson as session 33's launch guard: a check that only prints is not a check.
- **A separate launcher, with the guards SHARED rather than copied.** `xrsim-launch.ps1` calls
  `launch-game.ps1 -PreflightOnly` instead of reimplementing the another-BioShock-is-running and
  stale-command.txt checks, so the two launchers cannot drift apart. `boot.ps1 -Attach` is
  mandatory in sim mode: without it Steam does not know about a directly launched process and
  starts a SECOND game on the real runtime.
- **The compositor is the payload, and the projection pass is a real reprojection.** The layer's
  tagged pose and tagged fov both differ from the eye's, so compositing per eye means resampling
  through that difference - which makes a claimed-fov mismatch VISIBLE as magnification instead of
  something to be inferred. The capture JSON exposes it as one number, `derived.claimRatioH`;
  session 28's yaw warp was a 1.84x under-claim that took three sessions to pin down. First
  measured value on BS1 with stereo armed: 0.98.
- **This retires a documented limitation.** XR quad layers - the aim laser, the aim dot, the HUD
  panel - exist only in the compositor and have never appeared in a window screenshot, which
  TESTING.md has recorded as un-checkable outside a headset since M8. The sim composites and
  counts them, so "is the laser on screen" is now an assertion.
- **Fidelity is a model, and the FOCUSED policy is the sharpest edge.** Session 28's finding that
  VDXR will not re-grant FOCUSED to an app submitting nothing is one runtime's behaviour observed
  once. Baking it in as the only truth would manufacture confidence, so `focus policy
  vdxr|permissive` models both and the default is stated, not assumed. Standing rule: a pacing bug
  that reproduces in the sim is real; one that does not may still exist on VDXR.
- **No wait in the sim is ever unbounded.** Every wait takes a finite timeout, no lock is held
  across one, and step mode grants a frame after 30 s of starvation. An agent that walks away
  mid-step leaves a slow game, never a hung one - which matters because an unbounded stall on the
  present thread is precisely the failure this project has already been bitten by twice.
- **Compositing is off except on capture frames.** A test instrument that costs frame time becomes
  the pacing bug it was built to find.
- **What it deliberately does not model, and never will:** lens distortion, chromatic aberration,
  timewarp/reprojection, real display cadence, VDXR's Wi-Fi encode path. It proves geometry,
  content and protocol. Comfort, judder and world-scale remain the user's verdict in the headset,
  and still belong in the F10 overlay per the session-33 entry above.
### 2026-07-31 (session 34) - who owns the frame loop, and whose code a core fix may move

- **THE PER-GAME MODS STAY DECOUPLED; duplicate code is the cheap option (user directive).**
  Copying a BS1 behaviour into `bioshock2r/` beats promoting it to `core/` or parameterising the
  BS1 version. BS1 is the headset-accepted baseline, and a BS1 regression costs headset time even
  to *detect* - so an abstraction extracted mid-flight turns every BS2 change into a possible BS1
  regression. Consolidation is a dedicated session in the polish milestone. The same was said for
  the BioShock Infinite mod. **Corollary, and it shaped this session's fix: when a change to
  `src/core/` is unavoidable, it must be purely ADDITIVE - new state defaulted to today's
  behaviour, switched on by the adapter that wants it.** Detached pacing is core code that only
  BS2 turns on; BS1's path is byte-identical until BS1 opts in on its own test.
- **A DIAGNOSIS THAT THE TELEMETRY REFUSES IS NOT A DIAGNOSIS.** Session 33 concluded the frame
  handoff paced the game and wrote it into three documents. The evidence quoted in the same
  breath - `lastWait 0 ms`, `timeouts 0` - says the wait returned instantly and the handoff never
  reached its deadline, so neither call could have cost the measured 100 ms. The failure was not
  the guess; it was promoting a guess to a finding while the contradicting numbers sat in the same
  log line. **Before fixing a stall, time every phase of the path it is on**, and prefer a fix
  whose correctness does not depend on which phase turns out to be guilty.
- **An unfocused session must not own the game thread's frame loop.** The two duties are separable
  and belong on different threads: SUBMISSION (keep a live frame loop so the runtime can re-grant
  FOCUSED - session 28's requirement, preserved) and PACING (the game thread must never block on a
  runtime whose cadence is not the display's). Splitting them is what makes "keep submitting, stop
  waiting" implementable without choosing between a freeze and a permanent unfocus.
- **BS2 fills the headset eye by FOV, never by aspect - the exact opposite of BS1's policy.** BS2's
  law fixes `tanV` against a 16:9 reference regardless of backbuffer aspect, so a squarer
  backbuffer only narrows the horizontal and buys no vertical view at all; the FOV option is the
  only lever. BS1's square-backbuffer policy exists because its law is a true horizontal, where
  aspect *is* the lever and no FOV write is needed. Fourth never-copy instance after the ini keys,
  the cb0 offset and the FOV law - and the first where what would have been copied is a *policy*
  rather than a number or a formula.
- **The default-OFF rule for render levers is overridable, by the user, on the record.** `vrfov`
  ships DEFAULT ON on BS2 because the user asked for the outcome it produces ("I want the visual
  space to be the whole screen/FOV") after the defect was measured. The rule exists so an unjudged
  lever cannot silently change a headset-accepted configuration; it does not exist to overrule the
  person doing the judging. Flipping a default needs a measurement and an instruction, and this
  had both.
- **Ship the number that justifies the toggle NEXT TO the toggle.** The FILL THE VIEW control shows
  rendered fov, eye fov and the missing degrees. In-headset the user cannot read a log, so a
  control without its measurement is a control they can only judge by vibe - and "black bars"
  versus "38% of the eye's height is unfilled" are very different bug reports.
- **(2026-08-02, session 36) The fix for a race is removing the wait, not narrowing it - and the
  proof is a counter, not a soak duration.** BS2's stereo freeze was Draw's tail flush handshake
  doubled by the second draw; `wait2/s` (latch state sampled at second-flush entry) showed the
  INFINITE wait was entered on EVERY doubled frame at every resolution, so no timing knob could
  ever have made it safe. `reentry 1t` (BS1's flush-point hook, duplicated into bioshock2r with
  fresh constants per the decoupling rule) makes the wait structurally unreachable: `wait2/s == 0`
  by construction, measured. A backend selector (AlternateEye default) shipped mid-session as a
  floor and was flipped back to full-rate SR-on-1t the same day, on a green 10-minute soak - the
  selector survives only as `vraer` plus the `srdev` repro gate.
- **(2026-08-02, session 36) A watchdog episode the game recovers from is a load, not a wedge -
  permanence is the verdict, the watchdog is the cry.** Save loads and respawns legitimately stop
  presents >4 s, and the widened stall watchdog fires on them; the soak now gives every episode a
  30 s recovery watch (log growing = load, counted and reported; silence = the freeze, fail). The
  alternative - teaching core to distinguish loads - would have traded diagnostic stacks for
  silence exactly where stacks are cheapest. Lesson bought by -KillOnFail killing a healthy game
  the moment the user finished loading their save into it.
- **(2026-08-02, session 36) The simulator force-grants focus, so every session-state negotiation
  bug is INVISIBLE to it - VDXR state handling is only ever proven on the real runtime.** The first
  real headset attach since the detach lever landed found two in one evening: bring-up (a
  never-focused session must pump frames to REACH focused) and re-attach (VDXR never re-promotes a
  session submitting empty keepalives - it parks at VISIBLE/shouldRender=0 forever). Both were
  structurally unreachable in the sim and in flat soaks (no runtime = no session). Consequence for
  verification: state-machine changes get a headset smoke test at first opportunity, and "black
  void + VD environment" is read as BACKGROUNDED (foreground the app in the headset), not broken.

### 2026-08-02 (session 37) - the letterbox was the window, and resolution goes live

- **Identify the MECHANISM before bisecting its parameters.** The plan was a 4-5 boot aspect
  bisection to find "the squarest aspect BS2 renders full-height". One `GetClientRect` on the live
  window replaced the whole campaign: 1421 was never an engine constant, it was the chromed
  window's client height on this desktop, and the engine sizes its scene viewport to the client
  every frame. A bisection would have converged on a number that changes with the user's monitor.
  Rule: when a measured constant has no derivation, first ask what SYSTEM produced it - a
  parameter sweep over the wrong mechanism measures the lab, not the engine.
- **The BS2 resolution apply is LIVE and window-shaped; BS1 keeps ini+relaunch. Do not port
  either direction.** `apply_resolution()` orders the work: borderless window resize FIRST (the
  engine follows with its own ResizeBuffers and then writes its own lagging value into
  Shared.ini), ini persistence SECOND, a deferred re-verify THIRD (4 s, one rewrite) because the
  engine's resize-persist records the PREVIOUS size mid-transition - the mechanism that made every
  earlier "the write did not stick" report. A self-heal in the poll gate re-applies the fix when
  stereo is armed and the client is smaller than the backbuffer (every chromed boot's state);
  it is gated on `stereo_active()` so flat play is never touched, holds off 6 s around an apply
  so it cannot fight an in-flight resize, and is disabled under StartupFullscreen.
- **No anamorphic claim correction, still - and now none is needed.** Only a letterboxed state is
  anamorphic, the enforcement removes letterboxing, and the picker warns instead on aspects far
  from the eye's ~0.93. A symmetric correction was wrong anyway: BS2's band is bottom-anchored,
  so honesty would need an asymmetric angleUp/angleDown claim the single-hfov path cannot express.
- **A simulator default must be a measured value, not a published figure.** The sim's Quest 3 eye
  shipped as the spec-sheet guess h=55 v=48; the real VDXR device measures h=54 v=55. Wide-short
  vs square flips which branch of the FOV circumscription wins, so every FOV-derived sim number
  silently disagreed with the headset. Pinned to the measured values (the session-34 open item);
  the pinning mechanism is the mod's own `headset fov half-angles` log line.
- **claimRatioH is claim over EYE, and ~1.0 is only the eye-matched case.** With the FOV fill ON
  at 16:9 the mod over-renders honestly and ~1.8 is CORRECT there; the real per-config assertion
  is `measured == tan(law(option,aspect)/2)/eyeTanH` (verified to four decimals at native, both
  eye shapes). The session-36 guard line "claimRatioH ~1.0 at every shipped aspect" was written
  when render==eye was the only design point and is superseded by the law assertion.
- **BS2's menu background is a strict-gameplay ShockPlayer view, and that makes it a flat
  screening context.** The whole letterbox/live-resize campaign ran unattended against the menu
  scene (view classifier: GAMEPLAY; fgfov and auto FOV arm; full scene pipeline renders). The
  save remains the ACCEPTANCE context per the user's standing directive - the screening pass
  spends zero user boots, not zero user verdicts.

### 2026-08-03 (session 38) - teardown-aware crash handling, and why it gates on messages

- **Faults after window close begin are answered with one log line, no dump, and
  `TerminateProcess` - in CORE.** Evidence (ENGINE_NOTES bs2 session 38): BS2R faults on its
  own exit path on every close, with every mod hook skipped; vanilla just hides it behind
  CSERHelper. The alternatives were rejected: per-SITE suppression (an address list) breaks
  the "addresses live in per-game patterns" rule and the observed site VARIES (+0x4FF0FE,
  +0xC6C2C2, +0xC312D2, freed-vtable jumps); adapter-side-only handling cannot stop the core
  crash filter from writing dumps. Gating on WM_CLOSE/WM_DESTROY/WM_ENDSESSION (seen by the
  overlay's existing game-window subclass) is game-agnostic, needs no addresses, and cannot
  misfire during play. BS1 path impact: zero until a close message, and BS1 does not fault
  at exit - the change is purely additive pre-close, per the decoupling rule.
- **TerminateProcess instead of chaining post-close**: chaining let a foreign filter retry
  the faulting instruction 86k times (the user-visible "exception loop"). The user asked to
  close the game; after the close message, ending the process IS the correct semantic, and
  measured closes went from 5-9 s (vanilla) to 0.1-0.3 s.
- **`BVR_SKIP` + `BVR_VEH` ship as permanent diagnostics.** The whole root cause fell out of
  a no-rebuild subsystem bisect plus a first-chance observer; the next mystery gets the same
  levers for free. Both read once at init, inert unless set.
- **The teardown disarm in bioshock2r consumes `crash::teardown_seen()` by polling in its
  per-frame gates** (an atomic read) rather than a WndProc of its own or a new adapter
  interface method - no new threads-and-messages surface, no igame_adapter change, and the
  deferred-lane trap (posted work never applies once ProcessEvent stops) is avoided because
  nothing is posted.

### 2026-08-03 (session 39) - BS2 motion controls: the probe-first seam, and a lock-free rig

- **A GNames census settled the dispatch question instead of per-name guessing.** The
  fire-chain names' cached-index globals (Lane A) can only prove a name is REGISTERED, not
  that native code dispatches it; the census (every ProcessEvent dispatch's UFunction name
  index, deduped, printed via GNames) observes the whole dispatch surface at once. Two
  self-deriving steps keep it layout-assumption-free: the FName ctor falls out of the
  existing PlayerCalcView scan, and the UFunction name offset is found by scanning the
  LEARNED CalcView UFunction for its own known index (ambiguity is logged; garbage census
  text would name a wrong offset immediately). One boot, decisive verdict, and the
  instrument stays for every future by-name question on this game.
- **The seam hooks land on the C++ impls, telemetry-first.** Same verdict as BS1 but
  re-derived (0 PE hits over 6 live fires); the weapon impl was found by a vtable-slot
  census (classify every slot by `ret imm` + writes-through-pointer-args) rather than
  BS1's nativemap (which this build simply does not have), and the ability impl by a
  .text sweep keyed on the pawn loc/rot displacements the weapon impl proved. Hooks
  enable at boot but only COUNT until `vraim on` + a live ray source - the decal proof
  ran against the same build that ships.
- **bioshock2r composes hands at true geometry with NO render-lock domain** (per the
  session-39 brief and session 21's user-accepted verdict: the lock's own correction WAS
  the +-90 deg laser-vs-gun drift). The mechanism/policy split (bones.cpp/hands.cpp) is
  kept from BS1 so an actor-mode fallback stays one policy swap away, but the lock,
  lockgain, lockdgain and lockpull levers are deliberately not ported. Acceptance was
  measured, not assumed: aimRayMaxDevDeg constant across a five-station controller sweep
  on the first build.
- **The bone cluster is runtime-configurable (`vrbones cluster`), defaulting to the whole
  rig on the right controller.** The per-hand split needs the bone-name map; shipping a
  configurable cluster means session 40 derives indices by name and flips a default
  instead of rebuilding, and the flat instruments already measure the thing that matters
  (aim/model sync).
- **Session-39 code is a ZERO-core-diff feature.** Everything rides existing public core
  APIs (`get_hand_pose`, `set_laser`, `set_aim_dot`, xr_math, pattern_scan primitives) -
  the BS1-risk surface was never touched, which is what makes the deferred-BS1-regression
  policy safe for another session.
- **Session 40: the bone drive composes ON the authored pose, not over it.** The rigid map
  used to be `delta = qtc * conj(refQ_anchor)`, which makes the anchor's rotation become the
  controller's outright and throws the mesh's authored frame away. On BS2's rig that frame is
  ~81.6 deg off the view frame, and that discarded rotation WAS the "~90 deg constant offset"
  the first look reported. It is now `delta = qtc` - every cluster bone keeps its authored
  rotation and the whole cluster turns by the controller's rotation relative to the AHands
  actor (which carries the view rotation). A controller aiming where the view aims reproduces
  the engine's own pose exactly. This is equivalent to a per-cluster bake of `refQ_anchor`,
  but it is self-deriving: nothing to re-bank when a weapon, animation or rig changes, which
  is why it was preferred over the baked constant the session-40 plan called for.
- **Per-hand clusters are baked INDEX RANGES; the bone-name map is a diagnostic** (BS1's
  conclusion, re-reached on BS2). `vrbones names` exists to DERIVE the ranges once - it is
  never in the drive path. The anchor is the bone the attachment renders from (right = the
  weapon pivot 63, proven by driving it alone and watching the gun move), falling back to the
  wrist where nothing attaches (left = 7).
- **BS2 renders BOTH lasers and BOTH aim dots at once** (user decision, session 40): BS2 is
  natively dual-wield, so unlike BS1 there is no single "active hand". This is the session's
  only core change, and it is additive by construction - `set_laser_slot(1, ...)` /
  `set_aim_dot_slot(1, ...)` are new entry points that no BS1 path calls, slot 0 keeps the
  exact behavior `set_laser` always had, and the dot budget is SHARED (kMaxLaserDots across
  both beams) rather than doubled, so the compositor layer arrays do not grow. Worst case
  went from 11 layers to 12 of the 16 a runtime must accept; measured live at 11.
- **The input pump lives on the ProcessEvent lane, not CalcView** (BS1's site). BS2's menu
  never runs PlayerCalcView, and pad menu navigation is exactly where a pad drive has to work
  first. The lane already hosted the command poller for the same reason. It carries a
  re-entrancy latch because UpdateInput dispatches input events that re-enter the detour, and
  a nested command poll could flip `vrinput` or install hooks mid-pump.

### 2026-08-04 (session 41) - BS2: the animation-preserving drive, and profile scope

- **The BS2 bone drive composes on the ENGINE'S OWN evaluated pose, not a frozen
  reference** (`vrhands anim on`, default). Per driven bone the drive adopts the live
  bank's translation+quat ONLY when they stopped being our own last write (a bone
  entering the driven set adopts unconditionally; a bone masked by the other hand never
  adopts), then composes `q = qtc * animQ`. This is algebraically the "engine delta on
  top of the controller frame" without forming the delta, and it turns the engine's
  restamps - previously the left-eye flicker race - into the drive's input. The per-PE
  repaint became absorb-then-recompose on pass 1 (pass 2 and reapply stay verbatim so
  both eyes render one frame). THE STRUCTURAL RULE THAT MAKES IT SAFE: **the scale
  channel is never adopted** - the engine does not restamp scale, so the bank's scale
  bytes are always ours and a 48-byte adopt would compound `g_scale` geometrically
  (and could adopt arms-hide's zero). Scale always composes from the captured
  reference. The rigid session-40 drive remains as `anim off` (also the escape hatch
  for the returning idle sway, which BS1 deliberately froze; BS2 is not bound).
- **Uniform weapon scale lives on the WEAPON'S OWN SkeletonInstance** (holdable+0x430),
  scaling bone scale channels AND translations about the component origin - the AHands
  pivot-63 scale is inverse-decomposed by attachment math (the session-40 canister
  proof) and stays only as the `scaleweapon` fallback (default OFF). 1.0 = restore +
  fully hands-off.
- **BS2 per-weapon profiles carry the RIGHT hand + weapon scale only** (user decision,
  session 41): the left/plasmid hand does not change on weapon switches, so storing it
  per weapon would silently fork its tuning; it stays global in vrpreset until a
  per-PLASMID key becomes derivable. Everything else is BS1's session-21 shape with
  fresh constants (holdable hands+0x4B4, UClass vtable 0x11E71F8) and the four seeding
  rules preserved verbatim.

### 2026-08-04/05 (session 42, BS2 presentation lane)

- **Backbuffer-composite classifier mode is a per-game OPT-IN FLAG, not an
  autodetect**: BS2 draws gameswf straight onto a RENDER_TARGET-only backbuffer
  and tonemaps via an indexed quad, which no BS1 fingerprint matches. The flag
  (`hud::set_backbuffer_composite`, adapter init) widens the target gate and adds
  an indexed-tonemap check; default OFF keeps every BS1 code path bit-identical.
  Autodetection was rejected: a heuristic that can flip at runtime is exactly the
  class of core change the decoupling directive forbids.
- **Fullscreen screens ship GENERIC on BS2** (user decision): no per-kind
  special-casing - unknown kinds classify into the screen-only family (head-locked
  quad) or the overlay family (panel redirect + projection), and a one-shot
  edge-armed frame dump (`vrcine dumparm`, auto-armed for the bars edge at adapter
  init) harvests fingerprints during normal play for any kind that misroutes.
- **`postfx cine` (size-only fallback during cutscenes) ships OFF on BS2** -
  deviation from BS1, where it is a scoped workaround. BS2 renders a square-ish
  backbuffer, where the size-only rule is maximally degenerate (it matches the UI
  atlases); enable only if a headset session shows the floating-screen artifact.
- **BS1's stall-watchdog screen_only exemption was NOT ported**: BS2 has no
  watchdog thread to put it in (verified absent - its protections pause doubling
  rather than kill stereo). If a watchdog is ever added, the exemption ships
  inside it, same commit.
- **Pad-A menu activation is a BS2-LOCAL A->Enter scancode translation**
  (keybd_event, menu-context gated, foreground-checked) rather than a core
  button-mapping change: core's A=use/loot mapping is deliberate and shared. The
  known reach limit: BS2's pause menu starves the whole PE-tail service lane
  (poll + pump + menukey) - fixing THAT is its own future lane, not a mapping
  problem.
- **Engine-state writes on BS2 go through SCRIPT SETTERS called by name**
  (FindFunctionChecked + ProcessEvent through the engine's own trampolines, SEH
  isolated, effect-verified): the crosshair lane (ShockPlayer.DisableReticle /
  EnableReticle, default hidden per user ask) is the precedent. BS1's Exec SET
  seam stays BS1-only.
- **The XR-to-pad map is a per-game TABLE selected by an opt-in core enum**
  (2026-08-06, session 44, Infinite I7). A synthetic pad's only job is to land on
  the bindings the game already ships, so the table is a property of the GAME, not
  of the composer - but the composer lives in `core/vr/openxr_input.cpp`, which is
  compiled only under `BVR_WITH_OPENXR`, has no flat stub, and receives its XR
  handles from `openxr_runtime.cpp` at five narrow lifecycle points. **A bsi-local
  duplicate of the composing path was considered and rejected**: an adapter cannot
  see the action handles, so duplicating it locally would first require publishing
  raw XR control state OUT of core - strictly more invasive than the alternative,
  and it would put a second consumer on every action. So: `PadProfile` +
  `set_pad_profile`/`pad_profile` as one `std::atomic<int>` in
  `core/input/xinput_bridge` (default `Bioshock1`), and two `constexpr PadMap`
  tables beside the composer, one relaxed load per compose. This follows the
  established policy-atom precedent in that same file (`stick_deadzone()`,
  `ammo_mod()`) and the `set_pose_lag` opt-in contract. A single scalar rather than
  a struct of per-field atomics deliberately: a live A/B must never compose a
  half-switched pad, and a partially-populated POD would silently unbind controls
  to bit 0. The BioShock 1 table reproduces the previous literals exactly - that,
  not an assertion, is the inertness argument, and it was measured (see below).

### 2026-08-21 - the cinema screen is anchored where it opened, not where you recentred

Ported from the BRVR mod as the first item of the consolidation
(`docs/bioshock1/PORT-PLAN.md`).

The quad that carries menus, the map, the manual, machine flows, the hack board,
loading screens and cutscenes had two placements and each had a failure the
other did not. `g_space` with an identity pose put it at the recenter origin's
forward, so a player who had turned since recentring got a menu behind them.
`g_viewSpace` pinned it to the head, so it swam with every glance. Session 22
picked between them per screen type, which meant choosing which failure to have.

Anchor mode is the third option and is now the default: take the head pose once,
when the screen opens, flatten the forward vector to horizontal so the panel is
never tilted, and leave the quad there in world space until the screen closes.
The player can look around a stationary screen, which is what makes it readable.
It re-places on the next screen, on a recenter, and after 0.45 m of head travel
(headset off the desk, player stood up), and on nothing else - ordinary head
motion must never move it.

**Two details are load-bearing and both are somebody else's scar tissue.**

The yaw is `atan2(-fx, -fz)`, not `atan2(fx, -fz)`. The quad's visible face is
its local +Z and `R_y(yaw)*(0,0,1) = (sin yaw, 0, cos yaw)`, so the second form
builds the MIRROR of the head yaw: the panel sits askew by twice the yaw, in
opposite directions looking left versus right, and is dead straight at yaw 0 -
which is exactly why it shipped in BRVR and survived testing. Any test of this
code that does not turn the head cannot see the bug.

The pose comes from the unconditional `xrLocateSpace` at frame open, and the
anchor is not latched on a frame where that pose is invalid. BRVR anchored from
a variable only the gameplay path wrote, so every screen shown before the first
gameplay frame - the startup movies, the main menu - landed at the local origin
instead of in front of the player.

**Verified in the simulator, no headset** (`tools/xrsim-launch.ps1 -Game bs1`,
main menu, one quad layer throughout):

| Check | Result |
|---|---|
| Placed at all | `screen anchored at yaw 0.0 deg, head (0.00 1.60 0.00) m` |
| Survives a pure 50 deg head turn | quad pose `(0.000 1.60 -1.750)` **identical** before and after, so it is world-locked and not following the head |
| Re-anchors on travel | `screen re-anchoring - head moved 1.20 m from the anchor` |
| Re-anchors on the RIGHT heading | at head yaw 50 it logged `yaw 50.0` and placed at `(-0.141 1.60 -1.125)`, which is head + 1.75 m along the head's own forward to **0.0000 m**. The mirrored placement would have been 2.681 m away |
| Black around it | every capture reported `1 layer(s): quad`, no projection layer |

Not verified: the `vrscreen` console command. `command.txt` is polled from the
CalcView hook, which does not pump at the main menu, so the line was written and
never read. The parser is untested at runtime; the F10 overlay controls call the
same functions and are the primary surface.

### 2026-08-22 (session 63) - BRVR parity on size and height, and where parity is only a number

The user reported the weapon and hands reading tiny and asked for BRVR's scale,
which felt right. Comparing BRVR's shipped `dist/BioshockVR.ini` against these
defaults, most of the pipeline already agreed and the agreement was not luck:

| Knob | BRVR | Here before | Delta |
|---|---|---|---|
| World scale | `EyeSeparation=3.2`, documented "game units == cm" -> 100 UU/m | `worldScale=100` | same |
| Foreground lens | `2*atan(tan(fov/2)/(W/H)*1.3333)` | session-28 `k=(4/3)*(H/W)` | algebraically identical |
| IPD | 64 mm | 63 mm | negligible |
| Hand scale | `HandsScale=0.8` | `0.793` | ~1% |
| Weapon scale | `GunScale=0.8` | `0.760` | ~5% |
| **Camera height** | **`CameraHeightOffset=9` cm** | **0** | **the only large one** |

**The decision worth recording is what we did NOT claim.** Adopting 0.80/0.80 is
parity, not a fix: a 1% and a 5% change cannot be what a "reads tiny" report is
about, and saying otherwise would have made the next headset session measure the
wrong thing. The height was shipped at BRVR's 9 cm as the actual candidate, and
`HandsScale`/`GunScale`/`CameraHeightOffset` were added to `BioshockVR.ini` so
the real number can be found by dialling in one session rather than by a rebuild
per guess.

**Ini precedence is deliberately the opposite of the intent, and is logged.**
`Bioshock1RAdapter::init` runs before `arm_vr_preset()` -> `load_vr_preset_values()`,
so a saved `vrpreset.ini` overrides the ini keys. That preserves in-headset F10
calibrations, which is right, but in a headset it is indistinguishable from the
ini being ignored - hence the startup echo saying so on the same line. The clean
model (ini is the source of truth, F10 is a front-end) stays blocked on the F10
writer, which rewrites `vrpreset.ini` wholesale and drops unknown keys.

**Mechanism parity was deliberately NOT taken for the hands.** BRVR scales the
AHands actor's `DrawScale` (+0x2AC). The hands here are POSITIONED by writing
bone translations, so an actor-level scale would silently scale our own writes
with them; the cluster compression stays. The weapon is the opposite case - it
is positioned by the engine through the attach bone - so a skeleton-less
holdable (the wrench) now scales through the weapon actor's `DrawScale`, which
is the one place this mod did visibly less than BRVR. Headset-confirmed the
same day, which settles session 16's open question: the RIG actor's DrawScale is
inert through the fg path, the WEAPON actor's is not. It also corrected the
lane - the wrench is authored at 0.800, so a multiplier compounded to 0.64 and
the knob is now absolute, as BRVR's is. See `docs/bioshock1/ENGINE_NOTES.md`.

### 2026-08-22 (session 63) - the F10 panel in a headset

The panel was unreachable in VR: ImGui's default corner, 420x420 inside a
~2750x2850 backbuffer, unscaled font, mouse-only. Making it controller-driven
turned up four things that are worth recording because each one LOOKED like a
tuning problem and was not - and each cost a headset session to find, because a
clean build proves nothing about a runtime contract.

**R3 + L3 TAP opens the panel; HOLD (~0.6 s) recenters.** The chord already
existed as recenter-only; it is now split by duration. Recenter took the hold
because it is rare and deliberate. **This changes recenter for BS2 and Infinite
too** - they drain the same chord - so it has a `docs/PORT-CANDIDATES.md` row.

**Point with the right controller, click with RT.** `overlay.cpp` injects
`io.MousePos` between `ImGui_ImplWin32_NewFrame()` and `ImGui::NewFrame()` -
the only window where an injected cursor is not overwritten by the real one.
The maths is deliberately position-free: rotate the aim direction into the
HEAD's frame, divide out the forward component, scale by the eye tangents from
`fov_audit`. **If the cursor sits off the ray, expect a constant scale factor,
not a redesign** - the backbuffer holds the game's render at the game's FOV
while the compositor presents through the claimed FOV, and those only agree
because the session-15 lens match makes them.

RT is swallowed **for the game only** while the panel is up, in
`compose_synthetic` - deliberately NOT where the XR pad is published, because
the overlay reads that same published pad to get the trigger. Everything else
stays live, so you can still walk with the panel open.

**THE HALF-SCREEN CUTOFF: the panel was never too tall, it was being
guillotined.** ImGui's Win32 backend sets `io.DisplaySize` from the **window
client rect** (`imgui_impl_win32.cpp:410`, `GetClientRect`), and the DX11
backend sets the D3D **viewport** to exactly that
(`imgui_impl_dx11.cpp:113-114`). We draw into the BACKBUFFER, which in VR is
near-square (~2750x2780) - but Windows CLAMPS a window to the monitor, so on a
1440p display the client rect is roughly half that height. Everything below it
fell outside the viewport and was never rasterised, at a fixed ~50% of the eye
image regardless of what the window asked for.

Note the comparison that matters is **backbuffer vs CLIENT RECT**, not
backbuffer vs render resolution - those two are near-identical and neither
explains it. Two observations corroborate rather than contradict it: the cut is
at the same ~50% **on the flat monitor too** (expected - the backbuffer is
scaled into the window for display, so a viewport covering its top half still
covers the top half of what you see), and **UEVR shows the identical symptom**,
which is what you would predict from any mod that draws ImGui into a hooked
Present while the game renders a backbuffer larger than its own window. (Noted
as a symptom only - UEVR is all-rights-reserved and no code was consulted.) Fixed by pointing `io.DisplaySize` at the backbuffer after the
Win32 backend runs (and rescaling the real mouse into that space so the desktop
cursor still lands where it looks like it lands). This also makes the centring
maths and the controller pointer correct, both of which already assumed
backbuffer pixels. **The probe logs the proof once**:
`overlay: window client rect ... vs backbuffer ...` - if those heights match,
the cutoff was something else and this diagnosis is wrong.

**IMGUI INPUT MUST GO THROUGH THE EVENT QUEUE - three bugs, one cause.**
Reported: the cursor rendered in the RIGHT EYE ONLY, the trigger stopped
clicking, and the stick-drag moved the cursor without changing any slider.

Since ImGui 1.87 (vendored here: **1.92.8**) `io.MousePos`, `io.MouseDown[]` and
`io.MouseWheel` are DERIVED state, rebuilt from queued events inside
`NewFrame()` - `imgui.cpp:10535` is literally
`io.MouseDown[button] = e->MouseButton.Down`, and `imgui.cpp:808` lists
"Backend writing to io.MouseDown[] -> backend should call
io.AddMouseButtonEvent()" as obsolete. The first cut wrote the FIELDS. Position
survived by luck (nothing else queues a position event while the physical mouse
is still); the button writes were overwritten every single frame, because the
queue always carries the real button state. Hence a cursor that moved but could
not click or drag. Now `AddMousePosEvent` / `AddMouseButtonEvent` /
`AddMouseWheelEvent` throughout - including the real-mouse rescale, which was a
field write for the same reason.

The right-eye-only half was a second bug in the same function: it BAILED OUT on
a failed pose read, leaving the cursor position unset for that present. Each eye
is its own Present under SequentialReentry, so failing on alternate passes sets
the cursor on one eye and not the other. It now holds the last good position.

**The slider reset-to-aim: ImGui sliders position ABSOLUTELY on click.**
`imgui_widgets.cpp` computes `clicked_around_grab` and preserves the value ONLY
when the click lands on the grab handle - click the track and the value jumps to
the cursor. The synthetic click-drag therefore reset the slider to the aim point
on every re-engage. ImGui stores `HoveredId` but no hovered RECT, so the grab's
position is not discoverable from outside; pre-aiming at it is impossible.

The widget already implements the right path for keyboard/gamepad
(`ActiveIdSource == Keyboard || Gamepad`): it starts from the CURRENT value and
applies a relative delta, **one arrow-key press = 1% of the slider range**, and
never looks at the cursor. Reaching it needs `SetActiveID` plus an
`ActiveIdSource` override from `imgui_internal.h`; the keys themselves go through
the public event API. Activation happens in `UpdateSliderTweak()`, which must run
AFTER `NewFrame` (HoveredWindow is updated there) and BEFORE the sliders are
submitted. Step RATE is ours, not ImGui's repeat timer - the key is pulsed once
per due step (2-30/s, squared response), so stick pressure still means something.

**The runaway scroll was a QUEUE BACKLOG, not momentum.** `UpdateInputEvents`'
wheel branch is `if (trickle_fast_inputs && (mouse_moved || mouse_button_changed))
break;` - it stops draining. Trickling is on by default, and the ray cursor moves
every present (hand jitter guarantees it), so every wheel event was DEFERRED
rather than applied; they queued up and drained about one per frame, which kept
the panel scrolling after the stick was released.

Trickling exists for real hardware, where a burst inside one frame must be spread
so a fast click is not merged into a move. Our input is synthetic and already
frame-paced - exactly one consistent pointer state per present - so
`io.ConfigInputTrickleEventQueue = false` is correct here, not a workaround.

**Both stick speeds are now PER SECOND, not per frame.** The present rate swings
between ~100 and ~240/s (each eye presents separately under SequentialReentry),
so a per-frame step made drag and scroll speed a function of framerate. That is
also why the slider drag read about four times too fast.

**Polish from the first in-headset look**: the window scale was right but the
TEXT was too big (h/1080 = 2.37x), so the lift is now half that and there is a
**"UI text scale" slider** - it is a perceptual number and guessing it from
outside a headset is how the first attempt got it wrong. The panel is 0.45 of the
backbuffer tall and centred, so trimming pulls in equally top and bottom -
sized for a viewport that is now genuinely the full eye image. **Right stick scrolls it**, and the
right stick and trigger are both swallowed in-game while it is up, so a scroll
cannot turn you and a click cannot fire; the left stick still walks. **The
crosshair, aim laser and aim dot are hidden** while it is up - the beam used to
land on the panel and fight the cursor for the same pixels.

**Anchored (world-locked) placement was asked for and DROPPED as too big.** It
would mean the panel leaving the backbuffer for its own quad layer - render
ImGui to an offscreen RT, copy into a new swapchain, submit at the anchor pose,
and re-do the pointer as a ray/plane intersection. The `screen_place_mode`
anchor logic the cinema screen already uses is exactly the placement behaviour
wanted ("follows you, but a head turn does not drag it"), so a future attempt
should reuse it rather than invent one. The panel stays head-locked for now.

**The window opened small and off-lens because `io.IniFilename = nullptr`** -
ImGui never persisted anything, and there is no record of where it was left.
Interim default: centred, 42% x 52% of the backbuffer, with `FontGlobalScale`
tracking backbuffer height (a 1080p-authored font is sub-pixel at 2560 square).
**A probe logs the real geometry** on change, debounced 1 s, as fractions of the
backbuffer so the baked number is resolution-independent:

```
grep "overlay: window" %LOCALAPPDATA%\BioshockVR\bioshockvr.log | tail -3
```

Drag it where it belongs, read the fractions, bake them, drop the probe.


**The one thing deliberately NOT done: world-anchored placement.** Asked for and
dropped as too big. The panel is drawn into the backbuffer, which is what makes
it head-locked by construction; anchoring means giving it its own quad layer -
render ImGui to an offscreen RT, copy into a new swapchain, submit at the anchor
pose, raise the compositor's layer budget, and redo the pointer as a ray/plane
intersection. **`screen_place_mode`, which the cinema screen already uses, is
exactly the placement behaviour wanted** ("follows you as you walk, but a head
turn does not drag it") - a future attempt should reuse it rather than invent
one.

### 2026-08-22 (session 64) - scripted events are a separate question from cutscenes

The session was queued as "port BRVR's scripted-event detector, then the rotation
toggles". Researching parity first turned up a finding that reframed it, and it
is worth keeping because the same mistake is available to anyone reading BRVR's
graveyard.

**BRVR's *cutscene* detector was not the thing to port; ours is already the
stronger one.** BRVR's `docs/CONSOLIDATION.md` - written 2026-08-15 explicitly
for the merge into this repo, comparing its `9e56d58` against our `5bc5999` -
records that it spent three sessions and nine graveyard entries hunting a
cutscene signal while having one the whole time: `DrawHook_CutsceneBarsActive()`,
fed by the textureless 29-vertex letterbox-bar draw, **exported with zero callers
anywhere in its tree.** That same draw is `bvr::hud::bar_draw_active()` here, it
is consumed, and `wantCine` combines it with four more signals BRVR has none of:
the backbuffer letterbox pixel watch, the view-actor vtable test
(`body::is_gameplay_view`), CalcView staleness, and the rendered-vs-claimed FOV
mismatch, all under hysteresis. Its own verdict: *"the cutscene work is not a
port. It is wiring on this side, plus four hardening signals from his side."*

**So `wantCine` was not touched.** What was genuinely missing here is a different
question: a scripted SEQUENCE, where the world renders, the HUD may be up, and
the game is moving the player through an authored moment. That is BRVR's M7
window, CONSOLIDATION's Tier 2 item 6, and it ports cleanly because it is
engine-side and offset-driven rather than render-side.

**Held in the producer, not the consumer.** `scripted_window()` is the union of
the animation and forced-move signals held open for a settable delay, and the
hold lives with the signals rather than in whoever reads them. BRVR learned that
from the Little Sister crawl: the two signals failed to overlap for a single
frame at 231 CalcView/s, its camera hook released and re-armed the aim inside a
live scene, and the field ran 18.6 deg off for the next 58 seconds. Two correct
signals that failed to overlap, not a racy one. Consumers layer their own
policies on top; none of them get to decide when a scene is over.

**Zero core change.** The signals, the policy and the F10 section all live in
`src/game/bioshock1r/scripted.cpp`, and `git diff s63-bs1-comfort -- src/core/`
is empty. The panel section registers through the adapter's existing
`drawDebugUi()` chain, which is where every other BS1 section already lives.

### 2026-08-22 (session 64) - where the game's rotation actually reaches the player

The queued next-step was two toggles: freeze injected rotation during gameplay,
and drop injected vertical rotation during scripted events. Reading the CalcView
body before building either changed what got built, and the measurement is worth
recording because it retires half the work rather than doing it.

**During ordinary VR gameplay the game's pitch and roll already never reach the
player.** `camera.cpp` overwrites `rot->pitch` and `rot->roll` *absolutely* from
the head, and writes yaw as `gameYaw + headResidual`. Screen shake, weapon kick
and the auto-pan toward enemies all arrive as those deltas - and two of the three
axes are discarded by construction. This fell out of the head drive long ago and
had never been written down, which is why BRVR's `FreezeGameplayRotation`
(exactly that feature, and a real win *there*) read as a gap here.

**What is left of it is a yaw latch**, which is the one axis that is genuinely
risky: yaw feeds `driveYawOffsetRad` into `body::on_calcview`, which steers the
pawn, and BRVR's graves 12 and 13 are both about that coupling. Small prize,
sharp edge. **Deliberately not ported**; if it is ever wanted, it needs its own
session and its own headset run, not a rider on this one.

**The authored rotation does reach the player in exactly one case: when the head
drive does not run at all**, and CalcView's rotator passes through untouched.
That is a scripted or cinematic camera taking the view. It is the case the
comfort complaint was actually about, and it is the only case
`apply_rotation_policy` touches - so the control reads "when the game takes the
camera, follow its rotation: both axes / horizontal only / neither".

**The scene term is not decoration.** A menu also stops the head drive. Gating on
the drive alone would latch a menu's framing and log on every inventory open, so
the policy additionally requires `cinematic_hold() || scripted_window()`.

**Pitch is held absolutely, not differenced.** An authored pitch slew is removed
entirely and the horizon stays where the shot opened, rather than the shot
drifting by whatever it accumulated before the policy engaged.

**One control that already existed got a panel entry rather than a rewrite.**
`cine_drive()` (off / authored / authored+look) has been implemented and
preset-saved as `cineDrive` since session 29 and has only ever been reachable
from the `vrcine drive` console command. Authored+look - the head adding a
look-around delta on top of an authored shot - is squarely this section's
subject, so it is surfaced in the same F10 group. This is the third time a
session has proposed building something the panel already had.

### 2026-08-22 (session 64 part 2) - one movie name was three defects

Three separately-reported problems from one headset run turned out to be two
causes, and finding that out cost less than fixing any one of them would have.

**`PausePC.swf` is on the playing-movie stack for the whole of a bathysphere ride
and every scripted scene**, not only at the pause menu. `screens::panel_screen_up()`
matched it, that fed `publish_ui_pause(true)`, and `uiPaused` is an unconditional
term in core's `wantCine` - so the cinematic quad engaged. The user saw two
things and reported them as unrelated: the ride rendering on a head-locked square
with black behind it, and the head being completely dead during scripted scenes.
The second follows from the first because `cinematic_active()` also gates the
head drive. **Same cause, and the quad drops in the same millisecond as
`bathysphere off`, which is what made it findable.**

Two plausible co-causes were ruled out rather than assumed, and both would have
sent the fix somewhere useless: `g_cineStereo` already defaults **true** so the
FOV-mismatch term contributes nothing even though the mismatch is real (80
rendered against 100 claimed), and `screen-only` engaged five seconds *after* the
quad, making it downstream rather than a trigger.

**The fix is a gate, not a new mechanism**: a panel screen counts as a UI pause
only when no scripted window and no bathysphere ride is running. It is narrow by
construction - at a real pause menu neither signal is set - and it is only
possible at all because part 1's signals exist. This is the first thing those
signals bought.

**RETRACTION of part 1's "FreezeGameplayRotation is redundant here".** That call
rested on a correct measurement (the head drive overwrites `rot->pitch` and
`rot->roll` absolutely) and an incomplete conclusion: yaw is not overwritten, it
is composed as `gameYawUnits + residualUnits`, so everything the game puts into
yaw reaches the view. The user reported exactly that - screenshake from world
events and being panned onto groups of enemies - and it is now built.

The lesson is worth more than the feature: **"two of the three axes are already
discarded" is not "the feature is redundant".** The one surviving axis was the
one the complaint was about. A per-axis audit owes a per-axis conclusion, and
this one stopped a step early.

**The freeze absorbs the delta rather than clamping the value**, so the view
declines to be turned instead of snapping back to a held heading. It is
camera-only by the user's explicit choice: aim and body still turn with the game,
so the crosshair can sit off centre after a large shake. The alternative -
steering the body to match - would write the aim field during events the game may
be steering, which is BRVR's graves 12 and 13, and was rejected as a bigger risk
than the symptom. The offset is bounded at 60 degrees and logs if it gets there,
because an unbounded offset is an unbounded divergence between where you are
looking and where the pawn is facing.

**The both-edges rule arrived through a third door.** BRVR's rule is that a
reference latched inside a scripted window must be dropped on both edges, or the
second scene of a session differences against a value left from the end of the
first. Here there is an additional gap BRVR did not have: the freeze filter only
runs while the head drive runs, so a menu or a cutscene stops calling it
entirely, and the yaw it would difference against on the way out is from before
the scene. Absorbing that would swallow the whole turn the scene just made. The
filter re-arms whenever more than 250 ms has passed since its last call - dt is
the only witness available that it was not being called at all.

### 2026-08-23 (session 64 part 2, corrected) - two bugs that shipped looking plausible

The first cut of the comfort work built cleanly, logged convincingly and did
almost nothing. Both causes are worth writing down because neither is visible in
a diff and both are easy to write again.

**GetTickCount64 cannot see a frame.** It has ~15.6 ms resolution and CalcView
runs at ~118/s, so most frames are 8.5 ms apart and the counter does not move
between them. The rotation freeze computed `dt` from it and treated `dt == 0` as
"I was not called last frame, re-arm the reference" - which fired on roughly
every other frame, re-armed constantly, and therefore never accumulated a single
unit of the offset the whole feature exists to accumulate. It logged its own
"freeze ON" line happily. **Anything differencing per-frame state needs QPC**;
`GetTickCount64` is fine for the throttles it is used for elsewhere in this tree
and wrong for anything at frame cadence.

**A comfort filter must not live inside the head-drive block.** Both the freeze
and the scripted-scene turn were applied inside `if (loc && rot && driveHead)`,
which is skipped precisely when the game owns the camera - i.e. during the
scripted scenes they exist to handle. The right stick did nothing during a scene
because the code that would have turned the camera was never reached. The
accumulators now advance in `scripted::observe()`, which runs on every CalcView
unconditionally, and the result is applied in the unconditional `if (rot)` block.

**The general shape:** a feature gated on a condition that is false exactly when
the feature is needed. Worth checking for directly whenever a comfort or
scripted-scene behaviour is added, because it reads as correct at every step.

### 2026-08-23 - the bathysphere quad has a SECOND trigger, and it is `stale`

Suppressing the `PausePC.swf` false pause was correct and is confirmed working -
`uiPaused=0` throughout a ride now, with the gate logging when it fires. **It did
not fix the reported black square**, because a second `wantCine` term takes over
about 30 s into the descent:

```
23:42:41.876  scripted: bathysphere ON
23:42:52.295  scripted: panel screen up during a bathysphere ride - NOT treating it as a UI pause
23:43:11.281  xr: cinematic quad ON (strict=1 stale=1 fovMismatch=1 screenOnly=0 uiPaused=0)
23:43:22.342  scripted: bathysphere off
```

`stale=1`: CalcView stops being called. The camera line drops from 112 calls/s to
48 and then to 1, and the location jumps to a completely different world position
in between - this is the **arrival and map transition**, not the ride proper.
That is the session-22 finding ("scripted cameras bypass CalcView entirely")
arriving again, and the quad fallback is what session 22 built *for* it.

So this is not a gate problem and should not get another gate. Keeping stereo
through a CalcView-silent scripted camera is a render-side question - the adapter
cannot even refresh `publish_gameplay_view` during it, because that publish is
itself driven from CalcView. It needs its own session and a render-thread path
that still runs while the game thread has gone quiet. **Recorded, not bodged.**

### 2026-08-23 (session 64) - the turn axis is not on the composed pad

The gameplay rotation freeze shipped fixed and was still wrong in a way worth
recording, because the reasoning that produced it was sound at every step.

It excludes itself while the player is turning, which is what lets it hold the
game's rotation back without touching the stick. It read that stick from
`last_composed_sticks()` - the value the game is about to receive - which is the
obvious place and the wrong one. **Anything that claims the turn axis zeroes it
there**: snap turn sets `out.rx = 0` outright and keeps the step for itself, and
the F10 overlay zeroes it so scrolling a menu does not spin the view.

So "is the player turning?" answered NO permanently, the freeze absorbed the
player's own turn along with the game's, and the view could not be turned at all.
Reported as *"the view is fixed and turning the camera does nothing, like how the
cutscenes were before this fix"* - the user recognising the symptom of a
different bug, which is what made it findable.

**Read intent from `last_xr_pad()`, not from the composed pad.** The composed pad
answers "what will the game do"; a comfort filter is asking "what did the player
ask for", and those differ by design wherever the mod claims an axis.

### 2026-08-23 (session 64) - the rig during a scripted scene

Two halves of one idea, both defaulted ON after a headset session.

**The engine owns your hands.** A scripted scene now joins the session-29
cinematic gate: the controller stops driving the bone cluster and `bones::release`
hands the skeleton back, so the authored animation plays and the rig cannot be
dragged around mid-scene.

**The rig hides while a scene has nothing for your hands to do** - i.e. while
`scripted_window()` is open and `scripted_anim()` is false, which is exactly the
forced-move phase where the game is walking you into position - and comes back
the instant an animation starts, because that animation is what you are meant to
be watching.

**Hidden by the actor's DrawScale, never by collapsing bones.** BRVR's reason
transfers exactly: this file clears the skeleton's dirty byte so the engine does
not re-evaluate over our writes, so a collapse would freeze the very bone array
anything watching the rig would need to read. DrawScale leaves the array alone.
It never writes exact zero either - the attach path inverse-decomposes chain
scale, the same division that makes bone 43 untouchable.

**This is cheaper here than in BRVR, and the reason is the signal.** BRVR had to
infer "is an animation playing" from rig MOTION (`ArmHide_HandMotion`, with a
threshold and a 4-second hold) because its own `CurrentlyExecutingScriptedHand-
AnimationSequence` flag never locked. Ours locked on the first run, so the gate
is the flag itself and there is no threshold to tune.

The unhide is applied ABOVE the gate's early return, deliberately. Session 29's
collapsed-hand bug was precisely a restore living inside code the gate had
already skipped, and the same shape would have left the arms hidden for the rest
of a scene.

### 2026-08-23 - defaults promoted from the headset session

`RotFollow::HorizontalOnly`, scripted-scene turn ON, engine-owns-hands ON,
hide-rig ON, and BS1 opting into `CineDrive::Off` so the head keeps steering
through a cutscene. The cine-drive default is set **from the adapter**, not by
moving core's default, because BS2 and Infinite have never been tested with it
and keep the authored behaviour they ship with today.

The gameplay rotation freeze stays default OFF - it is the one of these that has
not yet been judged working in a headset.

### 2026-08-23 (session 64) - the blur was a per-eye yaw disagreement

Reported as the biggest problem in the build: *"things in the distance at about
10-15 feet away become blurry/fuzzy when walking and then become clear after
stopping."* It was not a render change. It was where one line sat.

The comfort yaw (`scripted::yaw_adjust_units()`) was applied at the END of the
CalcView body, on the reasoning that it had to work in both the head-driven and
game-owned paths and the unconditional block reaches both. **That put it after
two things that had already read the rotator:** `apply_eye_offset` builds the eye
separation axis from `*rot`, and the stereo second pass replays a stash
(`g_srBaseRot`) taken inside the drive block.

So the LEFT eye carried the adjustment and the RIGHT eye did not. A per-eye yaw
disagreement is a double image, and a double image is most visible on distant
geometry with the head translating - i.e. **while walking**, resolving the moment
you stand still. Exactly the report.

It is now applied once, immediately after `gameYawUnitsRaw` is captured and
before anything downstream reads the rotator, so every consumer agrees.
`gameYawUnitsRaw` is deliberately captured first, so `body::on_calcview` keeps
steering by the engine's own value and the filter stays camera-only.

**The general rule:** in this CalcView body, anything that changes `*rot` must
land before `apply_eye_offset` and before the second-pass stash, or it ships as a
stereo artefact rather than as a wrong number. That is much harder to attribute
than a wrong number, which is why it is written down.

### 2026-08-23 - the freeze inverted head look, and the body transfer is why

*"Looking left and right is inverted, but up and down is correct when looking."*
The asymmetry is the whole diagnosis: `body::on_calcview` only ever transfers
YAW.

The M7.5 body transfer steers the pawn to follow your head, which moves the
ENGINE's yaw by that amount. One frame later the rotation freeze sees the engine
yaw move while the stick is centred, cannot tell it from the game turning you,
and absorbs it - cancelling exactly the amount the body had just followed. Turn
your head left, the view slides right. Pitch is untouched because the body never
transfers pitch.

`body::on_calcview` already returns the exact integer it took - the same value
`g_recenterYawUnits` absorbs, which is what makes that invariant a theorem rather
than a tolerance. `scripted::note_body_yaw(moved)` now feeds the freeze the same
integer, and it subtracts it from the delta before deciding what the game
injected. Exact both ways, nothing left over.

**The shape to remember:** a filter that classifies engine state as "the game did
this" must be told about every change the MOD itself caused, or it will fight its
own machinery.

### 2026-08-23 - the arm hide was written against a falsified mechanism

Removed, not left in place. It hid the rig with the hands actor's DrawScale,
which is BRVR's mechanism and correct there - but this repo measured **rig-actor
DrawScale as geometry-inert** in session 63 (ENGINE_NOTES: "rig-actor DrawScale
does not size geometry; weapon-actor DrawScale does"), which is why the arms did
not hide during the balcony sequence. The port contradicted this tree's own
measurement, which is the exact failure the "never copy a number across without
re-deriving it" rule exists to prevent, and the finding was already written down
in this repo one session earlier.

The mechanism that DOES work here is the bone-cluster collapse `hide_inactive`
uses - but its reference capture and restore both live inside `drive()`, which
the new "the engine owns your hands" gate deliberately stops calling during a
scene. So the hide needs either a collapse path that survives without the drive,
or a restore hoisted out of it the way session 29 hoisted the sleeve latch. That
is real work and it is queued rather than bodged.

### 2026-08-23 (session 64) - a VirtualQuery per read, 470 times a second

The per-eye yaw theory for the walking blur was WRONG - the fix shipped and the
blur survived it. Retracted; the placement change was a real correctness fix and
stays, but it was not the cause and should not be cited as one.

The better candidate, and it is squarely this module's fault: `read_u32` and
`read_ptr_at` in `scripted.cpp` called `bvr::pattern_scan::is_memory_valid()`
before every read. That is a **VirtualQuery** - a syscall that takes the process
address-space lock - and `observe()` does four of those per CalcView at ~118
CalcView/s. **~470 VirtualQuery calls a second on the game thread, every frame.**

This tree already has a rule against exactly that shape ("never add a per-frame
memory scan"), and the change walked into it sideways: each individual call reads
as a cheap bounds check, and only the total is a problem. The `__try/__except`
around the read was always the actual protection, and is the idiom the rest of
the adapter uses - `body.cpp`'s `read_rot` has no VirtualQuery either. The
pointer identity is checked once per change in `anchor_check`, which is where a
one-shot check belongs.

**A master switch now exists** (`scripted::set_enabled`, top of the F10 section).
With it off nothing here reads engine memory and nothing reaches the camera. It
is there because "did this module cause it?" was going to cost a bisect across
builds and headset sessions otherwise, and one checkbox answers it in a single
run. Any module that can plausibly be blamed for a perceptual symptom should have
one.

### 2026-08-23 - a default change cannot ship through a file that stores the old default

`CineDrive::Off` failed to become the BS1 default twice, the same way in different
clothes. First it was set in the adapter's `init()`, which runs BEFORE
`load_vr_preset_values()` - the preset simply overwrote it. Then it was applied
after the load but only when the preset had no `cineDrive` line, which does
nothing when the preset HAS one, and every preset saved before 2026-08-23 carries
`cineDrive=1` from the old default.

**A stored value that happens to equal the old default is indistinguishable from
a deliberate choice**, so no has-the-key logic can fix this. It needs a schema
version, which is the ordinary answer: `presetVersion` is written on every save,
and a preset older than the version that introduced a default has that key
discarded. Anything chosen after this ships is saved at the current version and
wins normally.

Worth applying to any future default change that touches a preset-backed value -
there are ~50 of them, and this is the first time one has moved.

### 2026-08-23 (session 64) - DrawScale3D (+0x2B0) hides the rig; +0x2AC does not

The arm suppression works, and the reason two attempts failed is one field.

Session 63 measured the SCALAR DrawScale (+0x2AC) as geometry-inert on the rig
actor, which is correct and is written down. s64 then read that as "actor scale
cannot hide the rig" and removed the feature. **DrawScale3D at +0x2B0 - the
per-axis vector immediately after it - is a different field and does work.**
Measured in BRVR (`Hands/ArmHide.cpp`, `kDrawScale3DOff`) and re-derived here
against the same actor.

The layout is the ordinary Unreal shape, a scalar followed by a per-axis vector,
and the two are emphatically not interchangeable on this build. The inert one is
now documented directly above the working one in `patterns.h` so the next reader
gets both halves at once - a falsified finding that is only half-stated is worse
than none, because it retires a whole mechanism instead of one field.

**One-shot per edge, not per frame.** This is an actor field, so unlike a bone
write nothing re-evaluates over it, there is no dirty byte to keep clearing, and
it does not fight the skeleton drive or its reference capture. That is what makes
it compatible with the "engine owns your hands" gate, which stops `drive()` from
running during a scene - the collapse path could not have been.

Two guards travel with it, both from BRVR and both earned: refuse to SAVE an
already-collapsed scale (restoring that would leave the hands invisible for
good), and drop the saved value WITHOUT restoring when the actor pointer changes,
because the old address may already belong to something else.

### 2026-08-23 - the walk direction owes the same yaw the camera got

Reported alongside the arms: "the walking direction is now going to the wrong
place", appearing the moment the rotation freeze became a default.

`publish_move_yaw_offset` rotates the movement stick by the head-vs-body error,
`residual - moved`, so stick-forward tracks where you are looking. The freeze and
the scripted-scene turn both move the CAMERA away from the pawn without moving
the pawn - that is the whole point of camera-only - so the true camera-to-pawn
angle became `residual - moved + yaw_adjust` and the published value was short by
exactly the freeze offset. The view looked one way and the walk went another.

Fixed by adding the same integer the camera was adjusted by, so the two cannot
drift apart by construction.

**The general shape, and it is the third time this session:** a camera-only
adjustment is never only a camera adjustment. Everything derived from the
camera-to-pawn relationship - the movement stick here, the aim ray, the
viewmodel - owes the same term. Enumerate those consumers when adding one.

### 2026-08-23 (session 64) - four wrong hypotheses on one symptom, and the method change

A report of "things 10-15 feet away go fuzzy while walking, sharp once you stop"
drew four consecutive explanations from me, each a plausible mechanism, each
wrong, each costing a headset run:

1. a per-eye yaw disagreement from the comfort adjustment landing after
   `apply_eye_offset` and the second-pass stash;
2. ~470 `VirtualQuery` calls a second on the game thread from `is_memory_valid`
   in the per-frame read path;
3. and two earlier ones in the same shape.

**Both fixes were real and both are kept** - the adjustment genuinely did belong
before the eye offset, and a syscall per read genuinely did violate this tree's
"no per-frame memory scan" rule. Neither was the cause. That is the tell: they
were defects found by *reading code and asking what could produce this feeling*,
which is a search that always returns something and rarely returns the answer.

**The disproof that mattered was a switch, not an argument.** A master enable on
the whole scripted module, unticked in a headset, left the blur untouched - which
cleared the entire module in one run after four rounds of theory had not. Any
module that can plausibly be blamed for a perceptual symptom should have one.

**The method change: bisect revisions, do not reason about mechanisms.**
`tools/build-bisect.ps1` builds any set of revisions into `build/bisect/<label>/`
via git worktrees, so the working tree is never checked out and swapping revision
is copying one DLL. Three builds - `main` (v0.8.2), the s63 tip, and HEAD -
localise a regression to a SESSION in three runs, and the F10 toggles
(`vrcam headbob off`, `vrstereo off`) split it further inside the guilty one for
free.

The general rule, written for whoever hits the next perceptual bug: **a symptom
described in feelings is not debuggable by reading code.** Get it bracketed to a
revision range first, by measurement, and only then read.

### 2026-08-23 - a diagnostic switch must announce what it takes with it

The master switch was added to A/B the blur and it worked. It also silently
disabled the arm hide and the hands gate, which share the module - so the next
report ("arms still don't hide") came from a run with the module off, on a build
that did not contain the arm fix at all. Two questions confounded by one switch.

It now says so in the panel, in colour, and logs once when a scene starts while
it is off. **A switch that changes more than its label implies costs exactly the
session it was built to save.**

### 2026-08-23 (session 64) - the pre-compensation was in the wrong ORDER, not the wrong shape

s63 ported BRVR's movement-stick deadzone pre-compensation and put the call in
`openxr_input.cpp`, at the point the XR stick is read. The head-relative rotation
that the compensation exists to protect happens later, in `xinput_bridge.cpp`'s
`compose()`. So the chain ran:

    deadzone -> PRECOMP -> int16 -> ROTATE -> [game's per-axis band]

and the compensation was computed for a direction the game never received. BRVR
does it the other way round and only when it actually rotated
(`Input/InputHook.cpp`). Modelled against the game's own 0.225 per-axis band, the
old order does not merely bias the walk - **for small head offsets it erases the
rotation entirely** (5 deg and 10 deg both arrive as 0.0), peaks at 18.5 deg of
error at partial deflection near 20 deg, and flips sign past 45. The new order is
exact at every angle and magnitude tested.

**The fix is the move.** `precomp_stick_deadzone` and its two atomics now live in
`xinput_bridge.cpp` beside the rotation, applied immediately after it and only
inside the `deg != 0.0f` branch; the ini keys and per-game defaults stay in
`openxr_input.cpp` and reach the state through `bvr::input::set_stick_precomp` /
`set_game_stick_deadzone`. The s63 ramp deviation is unchanged.

**Why this core change is safe for the games it was not judged on.**
`stick_precomp()` defaults false and only BioShock 1 opts in, from its own
adapter via `set_pad_brvr_defaults(true)`. With it off the lane is unreachable and
the composed pad is bit-identical, so BS2 and Infinite keep exactly the behaviour
they have. Recorded in `docs/PORT-CANDIDATES.md`.

**The lesson, and it is BRVR graveyard entry 14's verbatim:** *when a correction
is provably exact and the symptom survives, go and measure what the other side
received.* The published rotation angle was algebraically exact the whole time -
`residual - moved + yaw_adjust` matches the rotator writes term for term - and
two sessions were spent re-deriving it. Nothing was wrong with the number; it was
being applied on the wrong side of a distortion.

### 2026-08-23 (session 64) - a faithful reading of a real flag can still answer the wrong question

The arm hide was gated on `scripted_window() && !scripted_anim()` and never fired.
The log showed why: `hands+0x594` bit 2 was already set on the first frame of the
window, so the predicate was a correct reading of a correct offset - of *"is a
scripted sequence running"*, which stays true through the parts of a scene where
the hands sit perfectly still. The question actually being asked was *"do the
hands have anything to do"*, and no flag on that actor answers it.

Replaced by a measurement: `bones::hand_motion()` differences one engine-owned
wrist per CalcView, `scripted.cpp` thresholds and holds it. The mechanism
(`DrawScale3D`) was never at fault and did not change.

**Where the module boundary landed, and why.** The measurement is in `bones.cpp`
because it owns the skeleton; the threshold, the hold and the fail-safe direction
are in `scripted.cpp` because they are policy, F10-facing and preset-backed. Same
split this pair already used for the drive itself.

**Fail toward the failure nobody has reported.** When both clusters are ours
there is no honest wrist and the sampler returns "cannot answer". That routes to
arms VISIBLE, because the failure on record is arms hidden for a whole scene and
there is no matching record of arms shown for one frame.

### 2026-08-23 (session 64) - never write the field the engine is steering by

A scripted forced move steers the player by `Controller.Rotation`. The M7.5 body
transfer writes that same field every frame so the pawn's yaw follows the head.
The two had never met, because until s64 the head drive did not run during
scripted scenes - part 2's `PausePC.swf` fix changed that deliberately, to let
the player look around, and switched the transfer on inside scenes as a side
effect nobody costed. Headset result: look around during a scene and you land
"way off"; hold still and you land exactly right.

The transfer now stands down for `scripted_window() || bathysphere()`. The held
window is used rather than the raw `forced_move()` because BRVR threw a landing
3.7 m by letting an equivalent predicate break mid-scene.

**Two rules came out of it, and the second is the one with teeth:**

- **A feature that removes a gate is also a feature that arms everything that
  gate was incidentally holding off.** The PausePC fix was reviewed as "the head
  drive now runs during scenes". What it actually did was set `vrDriving` true
  inside scenes, and `vrDriving` is a precondition for machinery in three other
  files. Enumerate the consumers of a flag before flipping the flag.
- **The mod may steer the player, or the game may, and never both at once.**

### 2026-08-23 (session 64) - two mods, two routes to head-relative walking

Ported BRVR's `WalkDriftProbe` and it settled a question two sessions of theory
had not. On all 25 samples of a normal walking run: `pub -0.0`, and
`camYaw == pawnYaw`.

**The head-relative stick-rotation lane is inert here.** The body transfer keeps
the pawn's yaw under the camera, so the angle the composer would rotate the stick
by is always zero. BRVR does the opposite: it never writes the aim field and
redirects walking purely by rotating the stick, on the identity
`walk = aimFieldYaw + stickAngle + R` - which leaves aim, the weapon trace and
forced-move sequences untouched by construction.

Neither route is wrong, and this is not a call to switch: the field write is what
makes the *body* follow the head, which the stick rotation cannot do. But the
cost is now known and paid once - the scripted-landing gate above exists only
because we write the field.

It also retires a red herring: **`moveYawSign` scales a value that is almost
always zero**, so flipping it in a headset changes nothing. That is the expected
result and says nothing about whether the sign is right.

### 2026-08-23 (session 64) - a correlation you caused is not a measurement

The arm hide is gated on measured rig motion. Round 9 shipped a diagnostic that
logged the sampled bone's position beside the hidden flag, saw 212 bit-identical
samples while hidden, and concluded that hiding the rig freezes the bone array -
BRVR's M7-S5 latch through a new door. The gate was moved off motion onto
`forced_move()` on that basis, and it was written up as measured fact.

**It was not a measurement.** In that build `hidden` was computed FROM the motion
reading, so "hidden" and "motion is zero" were the same event. The data could not
have come out any other way, whatever the engine was doing.

Round 10 decoupled them by accident - the hide ran off `forced_move()` while the
sampler kept running - and the unconfounded answer was the opposite: 3 samples
while hidden, 3 distinct positions. `DrawScale3D` is a safe hide.

**The rule: before reading a correlation as causation, check whether your own
code closes the loop.** A diagnostic that observes a variable the system is
steering measures the steering, not the system. The tell was available in the
same dataset and was missed - the frozen value also appeared with `hidden=0`.

The useful finding was underneath it all along: 229 of 336 samples read raw
exactly 0.0000, because CalcView fires far above the animation tick rate. The
hold, not the threshold, is what this feature runs on.

### 2026-08-23 (session 64) - the arm hide, and four rounds of fixing the wrong thing

The arm hide took four headset cycles, and only the last one changed the thing
that was actually wrong. The rest are worth recording because the failure was
methodological, not technical.

**What was wrong:** hiding the rig by scaling the actor to `DrawScale3D 0.0001`
takes it out of whatever the engine animates, so the entire bone array freezes -
and the gate that decides when the arms come back reads that array. A one-way
door. **What was built instead:** re-flagging the dirty byte every frame, then a
timed re-check that showed the rig briefly, then logic to discard readings taken
across the frozen gap. Each addressed a consequence; none addressed the freeze.

**The fix was to delete, not to add.** Hide by collapsing the BONES with the
actor left at full scale: it stays in the render set, the engine keeps animating
it, the array stays live. That removed the re-check timer, its two sliders, two
preset keys, a latch state machine that `want_rig_hidden()` was carrying (and
which the F10 render thread was calling into - a standing race), and the
trusted-sample logic.

**Three method failures, each of which cost a cycle:**

- **A correlation you cause is not a measurement.** The first evidence for the
  freeze was 212 bit-identical bone positions while hidden - from a build where
  the hide was *computed from* the motion reading. The data could not have come
  out any other way. Before reading a correlation as causation, check whether
  your own code closes the loop.
- **Then it was retracted on a grouping error.** "3 samples, 3 distinct values"
  looked like the array staying live; those three came from three *different*
  hidden windows, and a freeze predicts exactly that, since each window freezes
  at a different pose. The question was always *within* one window.
- **Read-before-write is not sufficient when you write faster than the engine
  refreshes.** CalcView runs 118-240/s; the animation ticks well below that. The
  sampler has to recognise its own write and report it as stale.

**And one process note.** The tester said twice that the answer was already in
the reference mod's docs, and it was - `INVARIANTS.md` offers "hide through
`DrawScale3D`, **or measure something the write does not touch**". Three rounds
went into the first clause. The second clause was the one that applied.

### 2026-08-27 (session 68) - "stop adopting" is not "go back"

s67 replaced the movement threshold with the engine's own `Hands` state machine,
so recoil could be adopted and a reload refused by NAME instead of by size. The
read works. The policy on top of it shipped a defect that the threshold never
had, and the shape of the mistake is worth keeping.

**The defect.** When the state leaves the adopt mask the drive stopped adopting -
and stopping is not returning. `g_ref` kept whatever frame it last took, and
`Hands.uc` leaves `WeaponFiring` at the TOP of the recoil, not after the gun has
come back down. So the pistol froze at its recoil apex and stayed there until a
weapon switch. The ammo-out freeze and the shotgun's first reload are the same
bug through different doors: `Firing -> Reloading` is a mask exit like any other.

**Why the threshold never had it.** BRVR gates on `adopt = (now - lastBigDelta) <
holdMs`, so it keeps tracking for ~1.2 s past the animation's last big frame and
re-freezes on the SETTLED pose. The settle window was the whole mechanism, and
the state branch is what threw it away. That is the general lesson: a per-state
gate answers "may this animation reach the rig", and silently drops the answer to
"where does the rig go when it stops" - which the thing it replaced was quietly
handling all along. **When replacing a heuristic, enumerate what it was doing
incidentally, not just what it was for.**

**Why we did not just reinstate the settle window.** It would re-freeze onto
whatever the idle animation had drifted to, and `Holdable::GetIdlingHandsAnim()`
draws a WEIGHTED RANDOM entry from `IdlingHandsAnim[]` every time `WeaponIdling`'s
loop comes round (Holdable.uc:32). That randomness is precisely the "crosshair
moves randomly between shots" report s67 fixed, so BRVR's answer is not portable
here - its threshold gate never had a per-weapon crosshair riding on the pose.

**What was built instead.** A CANONICAL rest pose: one snapshot per holdable,
taken the first time the engine reports `WeaponIdling`, restored when an adopted
state ends. Once per holdable rather than per return-to-idle, which is what makes
it deterministic and what keeps the per-weapon crosshair true between shots.

Three details that are policy, not incidental:

- **Position and rotation only.** The scale rows stay with the `g_scaleWrote`
  pinning bank; restoring them would undo session 61's pin-vs-adopt architecture.
- **The edge is only trusted on a KNOWN state.** The engine parks `StateNode` at
  null between transitions, and reading that blip as "the animation ended" would
  abort a recoil halfway through it.
- **The return is BLENDED (120 ms smoothstep), not snapped.** The pose being left
  is a recoil apex; cutting straight to rest reads as a jerk, where easing is the
  recovery the animation would have played. 0 ms = snap, and the F10 checkbox is
  a live A/B back to the s67 behaviour, because this is a perceptual question and
  the simulator cannot answer it.

### 2026-08-27 (session 68) - a shared global behind a per-instance setting

The viewmodel's view-frame PLACEMENT (`g_viewFwdCm/RightCm/UpCm` in `hands.cpp`)
was a single global triple applied to both hands. The per-weapon profile system
added in s67 then wrote it on every weapon change, from the RIGHT hand's profile.

Plasmids are the LEFT hand and have no per-weapon profile, so nothing ever put
the value back. The tester's report is the exact signature of that shape:

> correct as I switch back and forth between the plasmids, but then they **both**
> get locked to another position when I switch to a weapon and switch back

Every clause is diagnostic. *Plasmid to plasmid is fine* - no weapon profile
applies, so nothing overwrites it. *Locked after a weapon round trip* - the
weapon's placement was written and there is no plasmid profile to restore it.
And **both**, which is the clincher: two plasmids cannot share a fault unless the
thing they share is one variable.

**The rule: when a setting becomes per-instance, every global it reads or writes
becomes a candidate.** s67 made placement per-WEAPON without making its storage
per-HAND, so the weapon lane silently acquired write access to the plasmid lane.
The sibling values had already been converted - `g_posFwdCm[2]`, `g_rotPitchDeg[2]`,
and the aim trims - which is why this one was invisible: every call around it
already passed a hand index, so the one that did not read as intentional.

The fix makes it `[2]` like its siblings, routes the ini through the existing
`store_hand_key()` (so a suffix-less key from an older file loads into BOTH hands
- exactly what the shared global used to mean), and passes hand 1 at all five
`aim.cpp` call sites, since `WeaponProfile` is a right-hand record by construction.

**Also settled here:** plasmid position and plasmid crosshair are GLOBAL across
all plasmids, not per plasmid (tester's call, 2026-08-27). That falls out of the
same structure - only the right hand has per-weapon profiles - so the left hand's
model offset, view placement and aim trim are each one value for every plasmid.
The F10 sliders' L/R radio is the whole tuning surface for them; the numpad tuner
stays the WEAPON hand's tuner, which is why it now writes index 1 rather than the
old shared index 0.

### 2026-08-27 (session 68) - the plasmid had no key, so it wore the weapon's profile

**The engine parks `Hands.CurrentHoldable` at NULL while a PLASMID is equipped.**
That one fact produced two defects that looked unrelated, and it is why the s68
per-hand placement fix - correct in itself - did not cure the reported symptom.

The proof is in the log, not in reasoning. Every plasmid switch prints bones'
`wscale rigid: released (holdable gone)`, whose test is literally `!hold`, and
prints **no `[aim] weapon profile ... applied` line at all**. `update_weapon_profile()`
turned that null into an EMPTY key, and `apply_weapon_key()` dropped an empty key
on the floor with `if (key.empty()) return;` - no log, nothing applied. So the
outgoing WEAPON's entire profile stayed live: aim trim, placement, grip, and
`animOn`.

Both reports fall out of that:

- *"the plasmids move to a different position after switching to a weapon and
  switching back"* - they were wearing the weapon's profile, because nothing had
  replaced it.
- *"the animations aren't playing for the plasmids"* - `bones::set_anim_allowed()`
  is one global, and the WRENCH sets it to 0. After a wrench, the gate stayed shut
  and nothing reopened it. The run that reported this shows wrench<->plasmid
  switching throughout.

**The rule: a silent early return on a "nothing here" value is a bug waiting for a
second meaning.** `key.empty()` meant "unresolvable" when it was written; the
plasmid then made it also mean "a plasmid is equipped", and the branch could not
tell them apart because it did nothing and said nothing either way. Both now have
a name - `"Plasmid"` is a real profile key, and the genuinely-unknown case logs
that the previous profile is still applied.

**One key for every plasmid**, not one per plasmid (tester's call, 2026-08-27), so
they share one position and one crosshair.

**And a method note.** The first fix this session went in on a diagnosis that fit
the symptom perfectly - a shared global behind a per-instance setting, which was
also real and is also fixed - but was never checked against a log. The log had the
answer the whole time, in a line about a scale lane that nobody would think to
grep for. **Read the run before designing the fix, and grep for the absence of the
line you expect, not just for the presence of the one you fear.**

### 2026-08-27 (session 68) - the default profiles were scrambled twice over

`seed_default_profiles()` wrote bare aggregate initialisers in the order given by
the comment above it - trim, pos, animOn, view, grip, model - while `WeaponProfile`
declares grip BEFORE view and `animOn` AFTER both. Every field past `posUp` was
silently assigned to the wrong member. The Wrench ended up with `animOn = -16.70`:
nonzero, so its animation gate read as ON, which is the one thing it must not be.

Underneath it, a stale duplicate block re-assigned all eight weapons with FIVE
values each. Aggregate init zero-fills the remainder, so grip, view, `animOn` and
the model trims all went to zero and the second block won. **A fresh install had
no recoil on any weapon and no placement on any of them.**

Neither bug could be seen by anyone whose `weapons.ini` already overrode every
field by name - which is every machine this has ever been tested on. The tester's
own tuning file was hiding the shipping defaults.

Now designated initialisers, which cannot drift out of order again. **When a
struct's field order is load-bearing for a literal, name the fields** - the
compiler is the only reader that will reliably notice.

### 2026-08-27 (session 68b) - a timer standing in for a state the engine reports

s68's rest-pose fix armed a fresh reference capture on a holdable change and then
tracked for a FIXED 1200 ms (`g_swaySettleMs`) before freezing. That timer was the
wrong instrument, and it produced three reports that looked like three bugs:

| Report | Mechanism |
|---|---|
| "the wrench position was super off with a massive pivot on launch" | animOn=0, so once the timer expired the state branch refused forever - and the rest capture was gated on `g_animAllowed`, so it never ran either. The reference was whatever the timer froze, mid-equip |
| "it did an equip animation but froze it before it finished" | the equip outlasted 1200 ms; adoption stopped part-way through |
| "it applied it to both plasmids" | `CurrentHoldable` is NULL for EVERY plasmid, so plasmid-to-plasmid is `null != null` == false. The holdable-change test never fired, so one bad pose served all of them |

**The engine already reports both edges we were guessing at**: `State::Equipping`
starts the window, `State::Idling` ends it. Tracking until Idling means an
animation longer than any timer still plays out in full, and there is no constant
to tune. The Equipping EDGE also arms the capture, which is the only signal that
can see one plasmid replace another - the holdable pointer is blind to it.

**Two lessons, and the first is the one that keeps recurring here.**

**A timer is a guess about a state.** If the system publishes that state, read it.
s67 built the `Hands` state machine reader precisely so animation decisions could
stop being inferred from magnitudes and durations - and then s68 put a duration
back in the one place the state machine was most directly applicable. A timeout
survives, but only as a backstop that LOGS when it bites; a silent expiry is what
made this hard to see.

**Gates must not be reused for questions they were not asked.** `animOn` answers
"does this weapon FOLLOW its firing animation" - the wrench does not, because a
swing animation fights manual melee. s68 also used it to gate "does this weapon
get a correct resting pose", which is a different question with a different
answer, and starving the wrench of a rest capture is what put the s67 defect back.
When reaching for an existing flag, check that the question is the same one.

### 2026-08-27 (session 68b) - the hand was inferred from the trigger, not read from the game

`active_hand()` decided which hand was live from the last bumper or trigger the
player touched. Both the crosshair and the viewmodel follow it. A plasmid SHOT
does not - it is routed to `Hand::Left` by pointer identity, from the learned
object map.

So switching to a plasmid **without firing it** left auto-hand on the RIGHT, and
two reports that sounded unrelated came out of that one gap:

- *"it seems that it shoots slightly up and to the right of the crosshair"* - the
  dot was drawn from `g_ray[1]` while the bolt left on `g_ray[0]`. Two rays, two
  trims, one crosshair.
- *"the other plasmid still had the wrong position and nothing fixed it"* - the
  plasmid was being drawn with the RIGHT hand's offsets, which are the equipped
  weapon's profile.

And it explains why one plasmid was fine and the other was not, with no
difference between them: the tester had been **firing** the good one.

**The engine knows which hand it is.** `Hands.uc` parks the rig in an `Ability*`
state while a plasmid is equipped, so `hands_state` now carries an `ability` flag
per row and `active_hand()` reads it instead of guessing. An explicit hand mode
still wins - this replaces only the inference.

**The rule, again, and it is the third time this session: a value the game
publishes should not be inferred from a side effect.** s67 replaced "guess the
animation from movement magnitude" with the state machine. s68 replaced "guess
the equip is over from a 1200 ms timer" with the same state machine. This
replaces "guess the hand from the last trigger" with it too. Every one of these
was a proxy that agreed with the truth most of the time, which is exactly what
made each one survive so long.

**Profiles now own a hand.** `profile_hand()` maps the `"Plasmid"` key to hand 0
and everything else to hand 1. Before this the Plasmid profile wrote the RIGHT
hand's trims and offsets - values a plasmid never reads - so tuning its crosshair
moved the weapon's ray and the bolt kept leaving on the untouched left one. The
seeded Plasmid row is correspondingly rebuilt from the tester's left-hand tuning
(`vrpreset.ini`'s `aimTrimL*`/`aimPosL*`, `hands.ini`'s `*L`).

**Migration note:** a `Plasmid.*` block written by the s68 build holds right-hand
values under a key that now means the left hand. Those entries must be deleted,
not carried - a stale one is not merely old, it is in the wrong frame.

### 2026-08-27 (session 68c) - Idling is where the idle animation STARTS

s68b replaced the equip-capture timer with the engine's own `Idling` edge, which
was the right direction and still landed too early. `Hands.uc` starts the idle
animation with `PlayAnimationOnChannelInstantEaseIn` - a BLEND - so on the first
`Idling` frame the rig is still easing out of the equip pose. Capturing there
captures a partial blend.

**And the blend rate differs per path.** `AbilityIdling` eases at 4;
`AbilityGenericIdling` eases at 8. A fixed capture instant therefore lands
correctly for one plasmid and wrong for another with nothing configurable between
them - which is precisely the report that made no sense for three rounds: *"the
other plasmid is still wrong"*, with both sharing one global profile.

The tester supplied the decisive clue by pointing back at their own earlier
message: the positions were *perfect* on the build whose 1200 ms timer happened
to outlast the blend, and wrong only the once it expired early. That is not an
argument for the timer - it is the observation that **the correct capture moment
is "after the pose stops moving", which the timer approximated and the state edge
does not.**

So the capture now waits for the pose to be STILL, measured on the same probe
bones and the same thresholds the idle-sway kill already uses - two consecutive
settled evaluations, bounded by the existing 4 s backstop. No new constant, and
it adapts to whatever rate the engine picked.

**The refinement of the rule this session keeps teaching.** "Read the state the
engine publishes, don't infer it" got us here, and it was right - but a state
edge answers *when the engine changed its mind*, not *when the consequences have
finished arriving*. Those are different instants whenever a transition is
animated. Read the state to know WHAT is happening; measure the geometry to know
when it is DONE.

**Regression fixed in the same pass.** 9c3fe50 pinned the numpad tuner to hand 1
on the reasoning that it is the weapon hand's tuner. Once `active_hand()` began
reporting 0 for a plasmid, that made the tuner silently inert while one was
equipped - "none of the numpad modes change anything for the plasmids". It now
follows `active_hand()`. The tester cannot type mid-session, so an in-headset
tuner that does nothing for half of what you can hold is not a small loss.

### 2026-08-27 (session 68d) - the engine evaluates the bone array 5% of the time

**Measured:** 7 `engineEval=1` against 127 `engineEval=0` in one run. The engine
re-evaluates the hands bone array on roughly **one frame in nineteen**, and
irregularly.

Everything in `drive()` that reasons about "the pose changed" runs only on those
frames, because the whole recapture block is gated on `engineEvaluated`. So a
condition expressed as *a count of evaluations* is really a condition on the
engine's scheduling, which nothing in this mod controls or observes.

s68c's settle detector required "two consecutive settled evaluations". With
evaluations that sparse, where those two land - after the equip ease or in the
middle of it - is a RACE. That is the whole of "it was really close earlier, but
it would just break at a certain point": nothing about the holdable differed
between a good capture and a bad one, only the timing did. It also explains why
one plasmid looked right and another looked wrong while sharing one profile and
one code path, and why FOUR successive fixes to identity, hand, profile and
capture-edge changed the symptom not at all - none of them was in the loop.

**The unit matters more than the threshold.** The fixed 1200 ms timer this
lineage started with was blunt and it froze mid-equip when an animation outran
it - but it was in WALL-CLOCK, and wall-clock is immune to the evaluation rate.
Replacing it with an evaluation count fixed the bluntness and imported a race.
The settle now gates on time (stillness must persist 350 ms, and never capture
within 200 ms of the switch) with the evaluation count kept only as a floor, so
"still" is always a comparison and never a single reading.

**The diagnostic lesson, which cost four rounds.** A symptom that is bit-identical
across changes to four different subsystems is evidence that NONE of them is
involved. That should have redirected the search after the second attempt; instead
each round produced a new plausible mechanism, and plausibility kept winning over
the fact that the evidence had not moved. **When a fix changes nothing at all,
the next step is to instrument, not to theorise again** - and the instrument that
finally answered it, `engineEval` on the ROLLCHECK line, had been printing in
every log the whole time.

### 2026-08-27 (session 68e) - the settle test was a no-op, and its log was lying

Two defects in the settle detector, both mine, both introduced while fixing the
thing they then hid.

**The threshold answered a different question.** s68d's stillness test reused
`g_swayAngThreshDeg` - the ANIMATION ADOPTION threshold, 25 deg, raised in s67
because BRVR measures real animations at 130-170 deg at the wrist. "Is this
movement big enough to be a real animation?" and "has the pose stopped moving?"
are not the same question and do not share an answer. With a 25 deg tolerance,
any two consecutive evaluations of a smooth ease read as "still", so the test
fired on the second evaluation whenever that landed - the race it was written to
close was never gated at all. Measured: a capture fired at **2.34 deg of drift**,
against a measured idle envelope of +-1.2 deg. The pose was still visibly moving.

Stillness now has its own absolute constants, set just above that idle envelope:
the pose is still when the only thing left in it is breathing.

**And the instrument was lying.** The log line read `g_capStillSinceMs` and
`g_capStillFrames` AFTER zeroing them, so every capture printed `held still
19246078 ms across 0 evaluations` - the tick count and a zero. That number was
sitting in the log through two rounds of diagnosis being read as evidence.

**An instrument that lies is worse than no instrument**, because it is trusted.
The tell was available and obvious - 19,246,078 ms is 5.3 hours and "0
evaluations" cannot coexist with a rule requiring at least two - and it was read
past because the line was mine and assumed correct. Read the numbers an
instrument prints for PLAUSIBILITY before reading them for meaning.

**On the hypothesis this round started with.** The measurement said `bones::drive()`
runs for one hand and `active_hand()` is trigger-inferred, so a plasmid switched
to but not fired could be undriven. The census refuted it: 29 samples of
`hand=0 abil=1` against 8 of `hand=1 abil=1`, with the second plasmid driven
continuously across its own switch. The hypothesis was wrong and the measurement
cost one integer on a line that was already printing - which is the correct ratio,
and the opposite of the four speculative fixes that preceded it.

### 2026-08-27 (session 68f) - why "reverting it" did not restore it

The tester asked the question that broke the deadlock: *why didn't reverting the
commit put it back?* Because the revert (5ecbd49) undid the HAND changes only.
The CAPTURE path had been rewritten four times underneath and none of it was
reverted, so "back to the good build" was never true.

**The good build was e8d938d, and its capture identity was `CurrentHoldable`
alone.** `CurrentHoldable` is NULL for every plasmid, so that identity had a
property nobody designed and nobody noticed:

| transition | holdable | capture? |
|---|---|---|
| weapon -> plasmid | actor -> null | YES |
| **plasmid A -> plasmid B** | **null -> null** | **NO** |
| plasmid -> weapon | null -> actor | YES |

Plasmid-to-plasmid never recaptured, so every plasmid shared ONE reference pose
and they all sat in the same place. That is exactly the report: *"it was almost
perfect earlier position wise switching between the two, but it was just breaking
when switching to weapons."* Both halves of that sentence are this table.
Switching between plasmids was clean BECAUSE nothing recaptured; the weapon
transition broke because it is the only edge where a capture fires, and therefore
the only place the capture's own quality can hurt.

422f735 then added `CurrentAbility` to the identity so one plasmid replacing
another would force a fresh capture. The reasoning was sound in isolation and it
destroyed the working half - it converted a shared pose into a per-plasmid pose,
which is a per-plasmid POSITION by construction, and the opposite of the "global,
not per plasmid" the tester had asked for two rounds earlier.

**Two rules out of this.**

**A revert must be defined against a BUILD, not a commit.** "Revert the hand
stuff" undid one axis while three others stayed rewritten, and the result was
then read as evidence about the hand. Before reverting to restore a known-good
behaviour, diff every axis that build differed on and say which ones are staying
changed and why.

**An accidental property can be the load-bearing one.** Nothing intended plasmids
to share a reference pose; it fell out of watching a field that happens to be
null for all of them. It was still the behaviour that worked, and "fixing" the
accident regressed the feature. When a change makes a previously-good behaviour
worse, suspect that the change removed an accident the behaviour depended on.
