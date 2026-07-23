# Project status

> Handoff file. Rewrite "Current state" and "Next steps" every session; append to the session log.

## Current state (2026-07-23, session 2, end)

**DR-4 fully retired and M2 user-verified on the Virtual Desktop path - both in one session.**
The user confirmed in-headset: big head-tracked screen on the Quest 3, gamma OK (the sRGB
swapchain pick is correct), distance/width sliders work, "VR enabled" checkbox falls back to
flat cleanly; and in-game: wobble, Z/X/Y offsets, yaw and the FOV override all visibly work,
with **no stutter, crashes, or input weirdness**. M2's only remaining box is the optional
Steam Link/SteamVR cross-check. The first-ever in-headset BioShock moment of this project
happened today.

`core/vr/openxr_runtime.cpp` runs the whole xr_hello32 flow in-process: instance at init,
lazy session on the game device with a 5 s retry (**connect Virtual Desktop mid-game and it
comes up without restarting**), xrWaitFrame pacing at Present head, backbuffer (overlay
included) copied to the quad layer at Present tail. The game pauses its boot sequence while
unfocused (see ENGINE_NOTES) - foreground the window in automated tests. The FName-chain scan is ported
to C++ (`core/hooks/pattern_scan.cpp`, generic + parameterized; attributed to itsloopyo MIT) and
resolves `eventPlayerCalcView` at **RVA 0x1BE7A0** on the first try (1 wide-string match, 1
xref, exactly 1 candidate). The MinHook detour is live: fires every frame (heartbeat 400-7800
calls/s - **call rate can exceed fps**, and it **fires at the main menu**), telemetry + camera
offset/wobble/FOV-override controls are in the F10 overlay via the new `IGameAdapter` seam
(`game/igame_adapter.h`, minimal: capabilities/init/setFov/drawDebugUi). The exe loads rebased
(ASLR, observed base 0x0FB20000) - the live-memory scan is relocation-transparent; RVA is the
stable identifier. The 1 Hz camera heartbeat is **default ON** during bring-up (overlay checkbox
turns it off). Installed build in the game folder == HEAD.

**User checklist:**
1. ~~M2 in-headset test~~ - PASSED 2026-07-23 (big screen, gamma OK, sliders, clean fallback).
2. ~~DR-4 camera controls in-game~~ - PASSED 2026-07-23 (wobble/offsets/yaw/FOV all visible).
3. ~~Stability~~ - PASSED 2026-07-23 (no stutter, crash, or input weirdness).
4. Open item (any time): note whether **Roll offset** visibly tilts the horizon in-game - the
   one control not explicitly reported; it de-risks M3 head-roll.
5. Optional (any session): Steam Link cross-check - set SteamVR as active OpenXR runtime and
   repeat the M2 checklist.

## Previous state (2026-07-23, session 1)

M0 complete and user-verified (F10 overlay, D3D11 FL 11_0 confirmed, LAA yes). DR-1 fully
retired: xr_hello32 (32-bit) ran a complete OpenXR session on VDXR 1.0.10 with the Quest 3
(60 frames, RTX 4060 LUID match) - M2 is unblocked. DR-2 done. Repo public at
https://github.com/mohamad-balouza/bioshock-vr. Em dashes banned repo-wide.

## Next steps

1. **M3: 6DOF head camera** - everything it needs is now proven: `xrLocateViews` at Present
   head, publish the predicted pose, add `onCalcView` to the adapter seam, drive loc/rot
   (incl. roll) from the HMD, force FOV to headset FOV, switch quad -> projection layer
   (same image both eyes first). Needs world-scale calibration (`worldScale`, ~50 UU/m
   starting guess) + recenter button in the overlay.
3. DR-3: RenderDoc x86 capture with the proxy loaded -> frame map into ENGINE_NOTES.md.
4. DR-7: force borderless/windowed via ini and check overlay/capture stability (relevant to M2:
   game currently runs windowed 1024x768 after the user's change).
5. DR-6: instrument DINPUT8/window messages during menu use (which input path do menus read?).
6. Optional anytime: rerun xr_hello32 with SteamVR as active OpenXR runtime (Steam Link path).

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

### 2026-07-23 - Session 2

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
