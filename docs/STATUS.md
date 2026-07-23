# Project status

> Handoff file. Rewrite "Current state" and "Next steps" every session; append to the session log.

## Current state (2026-07-23, session 2)

**DR-4 retired (code-verified live; user visual pass pending)**. The FName-chain scan is ported
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
1. F10 in-game -> "Camera debug": toggle **Wobble test** (vertical bob), drag **Offset Z**,
   **Yaw offset**, and **FOV override** - confirm each visibly changes the view (best in a
   loaded save, not the menu).
2. Report any stutter/crash with the hook installed (Debug build, ~7k calls/s at menu is fine).

## Previous state (2026-07-23, session 1)

M0 complete and user-verified (F10 overlay, D3D11 FL 11_0 confirmed, LAA yes). DR-1 fully
retired: xr_hello32 (32-bit) ran a complete OpenXR session on VDXR 1.0.10 with the Quest 3
(60 frames, RTX 4060 LUID match) - M2 is unblocked. DR-2 done. Repo public at
https://github.com/mohamad-balouza/bioshock-vr. Em dashes banned repo-wide.

## Next steps

1. **M2: OpenXR session inside the game** - port the xr_hello32 flow into `core/vr`
   (instance/session on the game's ID3D11Device), pace frames from the Present hook, game frame
   on a quad layer. First in-headset gameplay moment (Quest 3 + Virtual Desktop, user tests).
2. **M3 groundwork is now trivial**: drive CalcView from the HMD pose (the hook + FOV write are
   proven; add `onCalcView` to the seam when core/vr can supply poses).
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
