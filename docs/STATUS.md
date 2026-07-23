# Project status

> Handoff file. Rewrite "Current state" and "Next steps" every session; append to the session log.

## Current state (2026-07-23/24, session 3)

**M4 rung 1 (AlternateEye) code is in on top of the verified M3 drive.** `core/vr` now owns a
PAIR of backbuffer-sized swapchains (index 0 still serves quad + mono projection). With camera
mode on, the "AlternateEye stereo test (judders)" checkbox alternates which eye each game frame
renders: CalcView shifts the camera by `sign * IPD/2 * worldScale` along view-right
(`vr::current_eye_sign()`), Present-tail copies the backbuffer into that eye's swapchain and
stores that eye's located pose, and submission gives each eye its LATEST held image + stored
pose so the compositor reprojects the half-rate-stale eye (judder, not flicker; mono fallback
until both eyes hold an offset image). The sign is published AFTER submit and only flips when an
offset frame was captured, so the un-offset enable frame is never mislabeled (see ARCHITECTURE
decision log). New controls: IPD slider (55-75 mm, default 63), "Swap eyes" inverted-depth
diagnostic, `(AER eye L/R)` tag on the layer line, and a "head offset (UU)" telemetry readout
that turns the world-scale question into a number. Flat path re-verified live this session
(scan RVA 0x1BE7A0, hook + heartbeat, VDXR instance, quiet no-headset retry, clean exit).
**In-headset AER test pending - checklist below.**

**FOV swim calibration DONE (2026-07-24, 1920x1080) - the distortion is solved and the cause
is found.** The user locked the world solid at claimed hfov ~100 while the forced field read
137, and with force off (field 100) it stayed solid: the renderer caps hfov at ~100. The live
ini explains it - the remaster settings pin **`HorizontalFOV=100`** under
`HorizontalFOVLock=True`/`bHorizontalFOVLock=True` (see ENGINE_NOTES). Consequences landed:
"Force headset FOV" now **defaults OFF** (with the lock active it only poisons the claim; the
truthful auto-readback claim gives a solid world with no slider needed), the manual claimed-FOV
slider stays as a permanent fine-tune. The **black band at the bottom** the user reported is
honest missing pixels: a real 100x68 deg image cannot cover the headset's ~110x100+ view
(asymmetrically deeper below center - hence "bottom"); it closes only when the engine truly
renders wider. **Experiment in flight**: both PC FOV-lock flags flipped to False in the live
ini (backup `Bioshock.ini.bvr-bak-fovlock`) - if the PC+0xE0 write now reaches the projection
past 100, force-fov becomes real and the band closes. No verdict yet on AER parallax with the
solid world. Installed build in the game folder == HEAD.

**User checklist (FOV unlock test - flat screen first, no headset needed for 1-3):**
1. Launch, load into the game (not just the menu). F10 -> Camera debug -> "FOV override" ON.
2. Slider to 60: view should zoom in hard (proves the write works below the cap - baseline).
3. Slider to 140: **does the view get much wider than normal?** WIDER = the ini unlock worked.
   Same-as-100 = still capped (report; plan B is `HorizontalFOV=137` in the ini).
4. If unlocked, in-headset: camera mode ON, "Force headset FOV" ON, manual claim OFF. Expect:
   solid world AND the bottom black band mostly gone. Slight residual swim -> fine-tune with
   "Manual claimed FOV" (it now snaps to the current claim when enabled).
5. AlternateEye ON -> judge parallax (wrench/railings), then IPD, world scale (head-offset UU
   line), immersion - the M3 deferred judgments.
6. Optional (any session): Steam Link cross-check - set SteamVR as active OpenXR runtime and
   repeat the M2 checklist.

## Previous state (2026-07-23, session 2)

**DR-4 fully retired and M2 user-verified on the Virtual Desktop path.** In-headset: big
head-tracked screen, gamma OK (sRGB pick correct), sliders work, clean flat fallback. In-game:
wobble, offsets, yaw, FOV override all visible; no stutter, crashes, or input weirdness. M3
landed the same session and its 6DOF drive was user-verified (rotation, lean, turn, recenter);
immersion/world-scale judgment deferred to M4 stereo. `core/vr/openxr_runtime.cpp` runs the
whole xr_hello32 flow in-process: instance at init, lazy session on the game device with a 5 s
retry (**connect Virtual Desktop mid-game and it comes up without restarting**), xrWaitFrame
pacing at Present head, backbuffer copy at Present tail. The game pauses its boot sequence
while unfocused (see ENGINE_NOTES) - foreground the window in automated tests. The FName-chain
scan (`core/hooks/pattern_scan.cpp`, itsloopyo MIT attribution) resolves `eventPlayerCalcView`
at **RVA 0x1BE7A0** (exe rebased under ASLR; RVA is the stable identifier). The detour fires
every frame incl. main menu, call rate can exceed fps (heartbeat 400-7800 calls/s, default ON
during bring-up). Adapter UI flows through the `IGameAdapter` seam.

## Previous state (2026-07-23, session 1)

M0 complete and user-verified (F10 overlay, D3D11 FL 11_0 confirmed, LAA yes). DR-1 fully
retired: xr_hello32 (32-bit) ran a complete OpenXR session on VDXR 1.0.10 with the Quest 3
(60 frames, RTX 4060 LUID match) - M2 is unblocked. DR-2 done. Repo public at
https://github.com/mohamad-balouza/bioshock-vr. Em dashes banned repo-wide.

## Next steps

1. **FOV unlock test** (user checklist above): does the PC+0xE0 write reach the projection past
   100 with the ini locks off? If yes: force-fov becomes real - consider re-defaulting it ON,
   and the claim echo is then truthful. If no: plan B = set `HorizontalFOV=137` in the ini
   (static but sufficient for a fixed headset fov); plan C = FName-chain scan for the live
   settings object and write `HorizontalFOV` in memory (per-frame control, needed anyway if
   SequentialReentry ever wants per-eye asymmetric fov).
2. **Then re-run the M4 rung 1 parallax test** (AER on, IPD + world scale) - AER mechanics
   already verified: eye tag tracks, depth not inverted (sign attribution holds), no crash.
   The world is now geometrically solid at the truthful claim, so depth/scale are finally
   judgeable even if the fov stays 100 for a while (narrow but correct beats wide but warped).
3. After AER proves geometry: DR-3 (RenderDoc frame map) + DR-5 (double scene draw) unlock
   SequentialReentry - the real M4 bet (full-rate true stereo).
4. Still open from M3: cutscene cameras are head-driven too (may need a viewactor == pc
   guard).
5. DR-3: RenderDoc x86 capture with the proxy loaded -> frame map into ENGINE_NOTES.md.
6. DR-7: force borderless/windowed via ini and check overlay/capture stability (relevant to M2:
   game currently runs windowed 1024x768 after the user's change).
7. DR-6: instrument DINPUT8/window messages during menu use (which input path do menus read?).
8. Optional anytime: rerun xr_hello32 with SteamVR as active OpenXR runtime (Steam Link path).

## Open questions / blockers

- Does the game recompute PC+0xE0 FOV itself anywhere (level load, cutscenes)? We restore the
  saved value when the override toggles off; watch for stale-FOV edge cases.
- CalcView call rate >> fps at the uncapped menu - before M3, determine whether extra calls are
  benign re-entries (same frame) or distinct view queries; affects where per-frame XR pose
  sampling should live.
- Console availability in the current Steam build unverified - test Tab with `-allowconsole`.
- Adapter VRAM logs as "3072 MB" - DXGI_ADAPTER_DESC.DedicatedVideoMemory is a 32-bit SIZE_T in
  our process, so values ≥4 GB truncate. Cosmetic; ignore.
- itsloopyo's headtracking mod also installs as `xinput1_3.dll` - mutually exclusive with ours
  (install.ps1 backs theirs up automatically).

## Session log (newest first)

### 2026-07-23/24 - Session 3

- **M4 rung 1 (AlternateEye) landed** per the session-2 design handoff: per-eye swapchain pair
  in `core/vr` (index 0 still serves quad/mono), held stale image + stored per-eye pose with
  compositor reprojection, eye sign published after submit with capture-gated flip (the sign
  doubles as image attribution - the un-offset enable frame stays mono instead of being
  mislabeled), `vr::current_eye_sign()` consumed by the CalcView drive for the half-IPD
  view-right shift. New overlay controls: AER checkbox (camera mode only), Swap-eyes
  inverted-depth diagnostic, `(AER eye L/R)` layer tag, IPD slider, head-offset UU telemetry.
  Also fixed in passing: the no-OpenXR stub block was missing `set_rendered_hfov` (latent link
  error).
- **Flat smoke test PASSED live** (game launched/closed via Steam under standing permission):
  log shape identical to session 2 - scan 1 candidate at RVA 0x1BE7A0, hook + heartbeat at
  menu, VDXR instance up, quiet no-headset retry, graceful exit. In-headset AER test = user's
  next step (procedure in TESTING.md, checklist in Current state).
- **First AER in-headset test (still 1024x768)**: AER mechanics verified - `layer: projection`,
  eye L/R tag tracks per frame, depth NOT inverted (1-frame sign attribution holds, swap-eyes
  not needed), no crash. BUT the M3 distortion persists (center-stretch relaxing toward the
  periphery on slow head turns) and blocks parallax/immersion/scale judgment; IPD ruled out as
  the cause. Diagnosis: claimed-vs-rendered fov mismatch that the self-echoing readback cannot
  correct. Landed **"Manual claimed FOV" calibration slider** (claims an arbitrary hfov in the
  projection layer; swim stops when claim == truly rendered fov -> the locked value measures
  the engine's real fov); layer line now shows target/readback/claimed. Calibration procedure
  written into TESTING.md; deriving + baking the engine fov mapping = next step.
- **(07-24) Swim calibration SUCCEEDED and found the cause**: at 1920x1080 the world locked
  solid at claimed ~100 with the field forced to 137, and stayed solid at field=100 unforced ->
  renderer caps hfov at ~100. Live ini: remaster settings pin `HorizontalFOV=100` under
  `HorizontalFOVLock=True` (+ `bHorizontalFOVLock=True`) -> ENGINE_NOTES updated (the PC+0xE0
  readback echoes writes instead of the rendered fov whenever the lock clamps). User also
  reported the expected honest black band at the bottom (uncovered headset fov below center) -
  grows as claimed fov shrinks; closes only when the engine truly renders wider. Landed:
  force-headset-fov **default OFF** (truthful auto-claim = solid world out of the box), manual
  claim slider kept per user request. **Flipped both PC FOV-lock ini flags to False** (backup
  `Bioshock.ini.bvr-bak-fovlock`) - unlock verdict is the user's next flat-screen test.

### 2026-07-23 - Session 2

- **M3 second in-headset test**: 6DOF drive solid; Force-headset-FOV visibly changed the
  image; user cannot judge immersion/world-scale without per-eye stereo -> M4 AlternateEye
  promoted to next session's goal (full design in Next steps). A started AER refactor was
  reverted cleanly (session wrap; HEAD == installed build). Session totals: 13 commits -
  DR-4 complete + user-verified, M2 complete + user-verified (VD path), M3 code landed with
  diagnostics, M1 down to DR-3/5/6/7.
- **M3 first in-headset test + fix round**: drive worked; distortion/no-depth/scale reports
  led to: projection-readiness gate (mixed quad+drive state now impossible), rendered-fov
  readback claims (fisheye self-correction), resize-swapchain-recreate fix, layer/fov
  diagnostics in overlay + log, Force-headset-FOV escape hatch.
- **M3 code landed (same session, after the M2 pass)**: core/vr locates head pose + per-eye
  views at Present-head (VIEW-space xrLocateSpace + xrLocateViews), computes the circumscribed
  headset FOV, and swaps quad -> projection layer in camera mode. camera.cpp converts XR
  meters/quat -> UU/FRotator (conventions documented in-file, roll sign pre-validated by the
  user's slider test), drives loc/rot/FOV in the detour, adds World scale + Recenter. Pose
  crosses threads under a mutex; pull-based access (see ARCHITECTURE decision log). Flat path
  re-verified live. In-headset M3 test = next session's first item.
- PowerShell 5.1 gotcha hit: double quotes inside git commit -m here-strings mangle argument
  passing - use a message file + git commit -F for multi-line messages with quotes.
- **User verification pass (end of session): M2 VD path + DR-4 both PASSED.** In-headset: big
  screen appeared, gamma OK, sliders work, clean flat fallback. In-game: wobble, offsets, yaw,
  FOV override all visible. No stutter/crash/input issues. M2 lacks only the optional Steam
  Link cross-check; roll-offset visual check noted as a small open item for M3.
- **M2 stretch landed after DR-4**: `core/vr/openxr_runtime.{h,cpp}` ports the xr_hello32 flow
  in-process (instance at init; lazy session on the game device with 5 s retry; event pump;
  quad swapchain sRGB-preferred; Present head = waitFrame/beginFrame, tail = CopyResource +
  endFrame; kill switch + screen sliders in the overlay). Verified live: instance on VDXR
  1.0.10 inside the game, quiet no-headset retry, DR-4 hook and game unaffected. In-headset
  test = user's next step (TESTING.md M2 procedure written).
- Found + recorded: game boot pauses while the window is unfocused (foreground it in tests).
- **DR-4 landed**: studied itsloopyo's `memory.rs`/`engine_hook.rs` (MIT), ported the FName-chain
  scan as generic `core/hooks/pattern_scan.{h,cpp}` (module capture, wide-string/imm32 sweeps
  via VirtualQuery region walk, E8 -> 89 0D global extraction, CC CC CC 55 8B EC prologue walk,
  200-byte init-site filter), consumed from the new `game/bioshock1r/patterns.cpp`.
- Created the `IGameAdapter` seam (minimal, grows per milestone) + `Bioshock1RAdapter`;
  framework wires `game::init_adapter()` between MinHook init and the D3D11 hooks; every
  failure path fail-soft.
- Hook detour (`__fastcall` dummy-EDX): original first, relaxed-atomic telemetry, offset/wobble/
  FOV-override application, one-shot first-fire log + 1 Hz heartbeat (default on). Overlay draws
  the adapter section through the seam.
- **Smoke-tested live twice** (game closed/relaunched via Steam with user's standing permission):
  scan resolved RVA 0x1BE7A0 both runs (rebased base 0x0FB20000 - ASLR active, scan
  relocation-transparent), hook fired at menu, heartbeat 400-7800 calls/s. Recorded in
  ENGINE_NOTES: fires at main menu, call rate >> fps, `AActor**` signature correction, FOV 100.0
  read live.
- 4 code commits + this docs commit pushed incrementally to main.

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
