# Project status

> Handoff file. Rewrite "Current state" and "Next steps" every session; append to the session log.

## Current state (2026-07-24, session 4)

**The FOV problem is fully closed, flat-verified end to end.** The live settings object
(`UShockUserSettings`) is located at runtime by scanning the heap for its fixed-RVA vtable
(no stable static pointer exists - ENGINE_NOTES), its int32 `HorizontalFOV` at +0x8C is
what the renderer consumes EVERY FRAME, and:
- **Auto-claim**: the CalcView detour reads it per frame and feeds the projection claim -
  the manual claimed-FOV slider is no longer required (kept as an override).
- **Write past the cap**: `gfov <deg>` (command/overlay slider) writes it per frame with
  save/restore; **flat-verified rendering at 137** (the options UI's 130 is UI-only, no
  code clamp; monotonic img-diff 117 -> 130 -> 145). "Force headset FOV" now writes this
  real control when VR-driving.
- **USER-VERIFIED IN-HEADSET (2026-07-24, same day)**: auto-claim solid with the manual
  slider untouched, and game-FOV write at 137 "very good" (lands on the ~137 headset
  target). Both checklist items passed on the first try.

**DR-3 done in-tree** (RenderDoc never needed): new `core/gfx/frame_inspector` hooks the
context vtable's draw/clear slots; `dumpframe full` writes a one-shot frame dump (RT descs,
VS b0 readback, callstack RVAs, auto-summary + lifetime call census). Findings in
ENGINE_NOTES "D3D11 frame map": HDR R11G11B10 main pass + D24S8, half-res effects pass,
shadow pair, view-proj matrix in VS b0 bytes 128-191 (fov-scaling cross-check EXACT), and -
the headline - **the renderer is a command queue** (executor 0x61C8E0 / drain 0x61CAE0 /
frame root 0x61D0F0, all byte-verified). SequentialReentry must therefore re-enter the
command BUILD, not the drain - that redefines the DR-5 probe (next session, frame root
first).

**New tooling this session** (TESTING.md has workflows): value scanner + poke/ptr/hexdump/
strscan commands behind the command seam (found the settings object via option-change
narrowing + poke A/B), `game-cmd.ps1` (focus-safe command writes), `img-diff.ps1`
(automated A/B verdicts). Debug-CRT gotcha recorded: sprintf_s asserts modally on overflow -
use _snprintf_s/_TRUNCATE for untrusted bytes (froze the game once).

**Known flake (unresolved, low-rate)**: one boot crashed 0xC0000005 at
`bioshockvr.dll+0x30BE5` during init (before the SEH guards landed on the vtable sweep;
has not recurred since). If it recurs, the crash filter now logs module+RVA + fault addr -
symbolize against the build PDB.

**User checklist - COMPLETED 2026-07-24 (both items passed, see Current state):**
1. ~~Auto-claim check~~ - solid with the manual slider untouched.
2. ~~FOV 137 in-headset~~ - "very good".
3. Still optional any session: Steam Link cross-check; 4:3 resolution experiment
   (see session-3 notes).

## Previous state (2026-07-23/24, session 3)

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

**M4 rung 1 USER-VERIFIED (2026-07-24): "parallax and other stuff are very nice."** The
winning configuration: the remaster's **FOV video option at 130** (its max) + the overlay's
**manual claimed-FOV slider at 130** (matching), VR camera mode + AlternateEye on. The user's
verdict: "everything is very good" - solid geometry, real parallax, depth not inverted. M3 is
now fully ticked too (6DOF verified session 2, geometry verified today). The whole fov saga is
resolved and recorded in ENGINE_NOTES: the PC+0xE0 field is telemetry-only (renderer never
reads it - proven by automated A/B screenshot sweeps via the new command-file seam +
tools/game-shot.ps1), the video option is the only real control, and the claim must match it
by hand until the settings object is scanned. Ini FOV locks back at stock True.

**Open item from the verification**: the user suspects the **IPD slider may not be doing
anything perceptible** - deferred by their choice. Note for that investigation: at world scale
50 UU/m, the 55-75 mm range only moves each eye by ~0.5 UU total, and perceived depth scale is
driven by the worldScale-to-IPD ratio - so a worldScale miscalibration would mask the slider.
Verify with an exaggerated test value (e.g. temporarily widen the slider range) before
concluding it is broken.

**User checklist (optional, any session):**
1. Steam Link cross-check - set SteamVR as active OpenXR runtime and repeat the M2 checklist.
2. If the thin top/bottom black bands bother: try the highest 4:3 resolution at FOV 130
   (vertical coverage becomes complete at a sharpness cost).

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

1. ~~In-headset checklist~~ - DONE same day, both items passed.
2. **DR-5 hook probe - next session's main focus**: command-gated hook on the frame root
   (RVA 0x61D0F0; command-queue architecture in ENGINE_NOTES). First just pass-through +
   soak, then count CalcView calls inside it (answers the per-frame-vs-per-view question),
   then find the command BUILD seam (what to re-enter for a true second scene render -
   re-entering the drain redraws nothing). Yaw-delta double-render once the build seam is
   identified.
3. If the init-crash flake (bioshockvr.dll+0x30BE5, one occurrence, pre-SEH-guards)
   recurs: the crash log now prints module+RVA - symbolize against the PDB and fix.
4. Still open from M3: cutscene cameras are head-driven too (may need a viewactor == pc
   guard).
5. DR-7: borderless/windowed stability; DR-6: menu input path (note: tools/game-click.ps1
   synthetic clicks DO work on gameswf menus - partial DR-6 answer already).
6. Optional anytime: Steam Link / SteamVR cross-check.
7. **Parked in M9 (user's call, 2026-07-24): IPD slider verification** - exaggerated-offset
   test first, world scale before IPD (perceived depth scale is the worldScale/IPD ratio).

## Open questions / blockers

- ~~PC+0xE0 FOV recompute~~ - moot: the field is telemetry-only (2026-07-24, ENGINE_NOTES);
  the engine re-stamps it to 100 on scene/controller changes, which is harmless to us.
- CalcView call rate >> fps at the uncapped menu - determine whether extra calls are benign
  re-entries (same frame) or distinct view queries; affects where per-frame XR pose sampling
  should live (matters more for SequentialReentry).
- Console availability in the current Steam build unverified - test Tab with `-allowconsole`.
  (Less urgent now: the synthetic-click path works for menus.)
- Adapter VRAM logs as "3072 MB" - DXGI_ADAPTER_DESC.DedicatedVideoMemory is a 32-bit SIZE_T in
  our process, so values ≥4 GB truncate. Cosmetic; ignore.
- itsloopyo's headtracking mod also installs as `xinput1_3.dll` - mutually exclusive with ours
  (install.ps1 backs theirs up automatically).

## Session log (newest first)

### 2026-07-24 - Session 4

- **(end of session) IN-HEADSET VERIFICATION PASSED**: auto-claim solid with the manual
  slider untouched; gfov 137 "very good" in the headset. The session-4 FOV work is fully
  user-verified; session closed with DR-5's hook probe as the next opener.
- **FOV endgame closed.** Built the value-scanner seam (memscan/mempoke/memptr/hexdump/
  strscan + int variants) and img-diff.ps1; narrowed 662 int candidates to 4 by having the
  user change the FOV option through the game UI between rescans; poke + screenshot A/B
  found the consumed copy; RTTI walk named it `UShockUserSettings` (+0x8C int32). Two traps
  documented: the ini value is an INT (float scans blind), and the one .data "static root"
  was a coincidental range hit later overwritten by floats - resolution is now a heap scan
  for the fixed-RVA vtable (cached, revalidated per call, SEH-guarded after one boot crash
  from unguarded reads during heap churn).
- **Auto-claim + gfov landed**: CalcView reads the option per frame into the projection
  claim (manual slider demoted to override); `gfov` writes it per frame with save/restore;
  Force-headset-FOV now writes the real control. Flat A/B: 137 renders wider than 130
  (UI cap is UI-only), restore returns to noise floor. In-headset check = user checklist.
- **DR-3 via in-tree frame inspector** (new core/gfx module; RenderDoc never installed):
  context-vtable hooks on draw/clear/SetRT slots, one-shot lite/full dumps with RT descs,
  VS b0 readback, triple-source callstack RVAs, auto-summary + lifetime census. Frame map
  in ENGINE_NOTES: HDR main pass, half-res effects pass, shadow pair, view-proj in VS b0
  bytes 128-191 (m00 scaled EXACTLY as 1/tan(hfov/2) between 117/137 dumps - independent
  proof the option IS the rendered fov).
- **DR-5 groundwork - architecture finding**: byte-walk of the draw-stack functions shows a
  render COMMAND QUEUE: executor 0x61C8E0 (`void __thiscall`, type id at this+0xC), drain
  loop 0x61CAE0 (site 0x61CD0D), frame root 0x61D0F0 (site 0x61D21E; its call rel32
  byte-verified to the drain). Consequence: SequentialReentry must re-enter the command
  BUILD, not the drain. Hook probe deferred to next session (fresh session; this one had
  two unrelated PC power cuts - user's electricity, not the mod).
- Debug-CRT lesson recorded in TESTING: sprintf_s asserts with a MODAL on overflow (froze
  the game mid-scan); value_scan switched to _snprintf_s/_TRUNCATE. Crash filter upgraded
  to log module+RVA + fault address. tools/game-cmd.ps1 added (focus-safe seam writes -
  the poller only runs while the game window is focused).

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
- **(07-24, later still) M4 RUNG 1 USER-VERIFIED.** With the video FOV option at 130 + manual
  claimed fov at 130: "everything is now perfect... the parallax and other stuff are very
  nice." M3 done-when ticked as well (6DOF verified session 2 + geometry verified today).
  Depth not inverted (swap-eyes never needed - the 1-frame sign attribution holds). Open
  follow-up parked by the user: whether the IPD slider has a perceptible effect (note in
  Current state: at 50 UU/m the 55-75 mm range is a ~0.5 UU change - verify with an
  exaggerated value before concluding). Session wrapped here.
- **(07-24, later) FOV endgame - field retired, real control found.** User reported the FOV
  override slider dead on flat with locks off. Built an automated harness in response: 1 Hz
  `command.txt` seam in the detour (`fov`/`offset`/`recenter`) + `tools/game-shot.ps1`
  (PrintWindow captures) + `tools/game-click.ps1` (synthetic menu clicks; navigated the menu
  and triggered a save load with them). Flat A/B sweeps: field writes 60-140 = pixel-identical
  frames in real gameplay AND the menu attract scene, under BOTH lock states -> PC+0xE0 is
  telemetry-only; DR-4's "override widens view" retracted (VR claim-side artifact - user
  clarified it was only ever observed in-headset). Locks restored True; direct ini
  `HorizontalFOV=137` also did nothing (out of range). **User found the real control: the
  remaster's FOV video option, range 75-130, visibly drives the render on apply.** Field does
  not mirror it -> manual claimed-FOV slider must match the option value for now;
  settings-object scan queued to automate the claim and attempt >130. Payoff run queued:
  option 130 + claim 130 + AER.

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
