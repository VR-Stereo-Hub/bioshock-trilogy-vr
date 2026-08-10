# Roadmap - BioShock Infinite

Milestones for the BioShock Infinite (UE3) adapter. Separate ladder from the BS1/BS2 M0-M10 in
[../ROADMAP.md](../ROADMAP.md), prefixed `I` so the two never collide.

Ordered **by engineering dependency**, not by desirability. The user's stated priorities appear as
acceptance criteria, not as ordering: on BS1 the resolution/FOV question could not be judged at all
until stereo ran, and two sessions were lost to mono screenshots that measured the wrong thing.

**RESTRUCTURED 2026-08-05 (session 38 wrap, user directive): the ladder now follows BS2's proven
order** - BS2 reached in-headset-accepted stereo on its third session and did lens/resolution
polish AFTERWARDS, driven by what the headset actually showed; controls, hands and presentation
then landed as three clean lanes (sessions 39-42). Infinite is better positioned than BS2 was:
the FOV law is already derived (I2), resolution is a solved lever (DR-I8, both lanes), and the
substrate is threaded/ring-buffered (DR-I5) - the same shape that made BS2's stereo cheap.
**Renumbering map** (session logs before this date use the old numbers): old I6 (stereo) -> I5;
old I5 (lens/FOV/config) -> I6; old I7+I8 (controllers, aim) -> I7; old I9 (viewmodel/hands) ->
I8; old I10+I11 (HUD, cinematics) -> I9; old I12 (set pieces/DLC) -> I10; old I13 (release) ->
I11.

Each milestone has a "done when" acceptance test that is a **measured downstream effect**, never a
confirmed write. Effort in focused sessions. Tick boxes as work lands; surprises go to
[../STATUS.md](../STATUS.md).

**Working branch:** `bioshock-infinite`, the integration branch for all Infinite work until it is
stable enough to merge to `main`. Per-session branches (`siN-inf-<topic>`) merge into it.

**Standing test constraint:** BioShock 2 development runs in parallel. Never run Infinite while
`Bioshock2HD.exe` is running - one game in the headset at a time. Building, installing and
packaging are unaffected. See [TESTING.md](TESTING.md).

**DLC is in scope** (user directive): bring-up on the base campaign, but every tuning and
calibration milestone must hold in Burial at Sea 1 and 2 and Clash in the Clouds.

---

## I0 - Recon and baseline (~0.5 sessions, mostly the user)

Goal: know what we are actually working with before any code runs inside the process.

- [x] PE identity: machine, ImageBase, ASLR flag, LAA, SizeOfImage, TimeDateStamp, CheckSum, sha256
      *2026-07-31 session 34: x86, fixed base 0x00400000 (no ASLR - unlike BS1/BS2), LAA yes.
      Full table in [ENGINE_NOTES.md](ENGINE_NOTES.md).*
- [x] Import table: injection vector confirmed
      *2026-07-31: imports XINPUT1_3.dll by **ordinal 2 and 3** - the existing `src/proxy/` works
      verbatim. Game IAT slot for ord 2 at RVA 0xCD4814, so BS1's IAT-hijack lane transfers too.
      d3d11/dxgi are NOT imported (loaded dynamically).*
- [x] Config-level survey: console/cheat surfaces, FOV levers, resolution keys, pacing settings,
      stereo section, full input binding vocabulary
      *2026-07-31: recorded in ENGINE_NOTES. Headline: the shipped `DefaultInput.ini` has a live
      "Debug binds" block (`god`, `ghost`, `preventdeath`, `walk`, `viewmode`, `shot`, and a
      `set <class> <prop> <value>` bind), which is a working cheat path the remasters never had.*
- [x] **User: launch the game flat once**, clear the first-boot flow, reach gameplay, save
      *2026-07-31: done. Generated the UE3 user config, which is what made the console question
      answerable. Saves are in Steam cloud userdata, not My Games. Latest is `TWN` (Columbia
      town), pre-combat.*
- [x] Confirm the live renderer is **D3D11**, not the D3D9 path
      *2026-07-31: **D3D11 CONFIRMED live.** `d3d11.dll` + `DXGI.dll` loaded, and decisively
      `nvwgf2um.dll` (NVIDIA DX10/11 UMD) is present while `nvd3dum.dll` (the DX9 UMD) is absent -
      `d3d9.dll` alone proves nothing since the launcher and Bink pull it in. Also confirmed live:
      `XINPUT1_3.dll` loaded (injection vector real at runtime) and **`GameOverlayRenderer.dll`
      loaded**, so expect BS1's Steam-overlay thunk problem and plan for the IAT-hijack lane.
      Module list needs **32-bit** PowerShell; a 64-bit host sees only the WOW64 shim.*
- [x] Harness verified against the live process
      *2026-07-31: `game-shot -Game bsi` captures real D3D content via PrintWindow (not a black
      frame - not guaranteed on D3D11), `game-cmd -Game bsi` writes a BOM-free command.txt, and the
      conflict guard was exercised in both directions (refused with BS2 up, allowed with it down).*
- [x] **Verify the debug binds fire**
      *2026-07-31, user-tested: **all six inert**, including the console on both `~` and `Tab`.
      Corroborated by positive control - `F9`=`shot` produced no screenshot anywhere. The binds are
      all present in the live `XInput.ini` and `ConsoleKey=Tilde` is set, so this is a dispatch
      failure in Infinite's custom `XCore.XPlayerInput` parser, not a missing command: `SHOT`,
      `SETRES` and `FULLSCREEN` all still exist as C++ `Exec` literals in the exe. Recorded as a
      dead end - do not spend another session on binds, launch flags or ini edits.*
- [x] Determine whether the console key can be enabled
      *2026-07-31: `ConsoleKey=Tilde` / `TypeKey=Tab` ARE set in the live ini and neither works.
      Closed.*
- [x] Establish the **test-loadout path**
      *2026-07-31: not via the console. `UXCheatManager` is in the shipped build, and the route in
      is reflection: `APlayerController::ConsoleCommand` (native, impl RVA `0x136070`), or better,
      straight to `AXPawn::SetWeapon` (`0x4F9ED0`), `AXWeapon::AddAmmo` (`0x5017D0`) and
      `AXPawn::AddInvulnerableFlag`. **Deferred to I2** - it needs the adapter to exist. Until
      then, a combat-ready save must be reached by playing to the raffle.*
- [x] Map `DLC\DLCA` / `DLCB` / `DLCC` to their titles
      *2026-07-31: DLCA = **Clash in the Clouds** (`DCLA_ZEP_Wave1..14`, `Arc_BlueRibbons`,
      `ARMORY_WEAPONS`), DLCB = **Burial at Sea Ep. 1** (`BookersOff`, `Attrium`, `Appliances`),
      DLCC = **Burial at Sea Ep. 2**.*
- [x] Offline: determine whether a **name-based native function table** exists
      *2026-07-31: **IT EXISTS** - 2647 entries, 8-byte `{ const ANSICHAR* name; Native impl; }`,
      names `<Class>exec<Func>` in ASCII (BS1's was 12-byte and UTF-16). BS1's fastest instrument
      ports. Enumerated offline; the dump stays in the scratchpad, findings in ENGINE_NOTES.*
- [x] Check whether **RTTI is present** in this build
      *2026-07-31: **present but useless** - all 270 type descriptors belong to third-party libs
      (Wwise, Bullet, FaceFX, Beast, std). Zero UE3/XGame classes; UE3 is built `/GR-`. The
      RTTI-walk lane that BS1 and BS2 rely on is DEAD here. Recorded as a dead end.*
- [x] Locate the camera seam offline
      *2026-07-31: `APlayerController::GetPlayerViewPoint` impl at **RVA 0x1E10C0** (thunk
      `0x129280`), thiscall, 2 stack args, `ret 8`, **13 native callers**. Every `exec` thunk
      checked has **0** callers, independently reproducing BS1's "hook implementations, not
      thunks". Control flow decoded (4 paths + a 4x4 SSE transform); `AActor::Location` `+0x44`
      and `Rotation` `+0x50` fall out of it. Unconfirmed live.*
- [ ] Offline: run UELib / UE Explorer against the Infinite packages in the gitignored
      `tools/uscript/` workspace (UELib explicitly supports package version 6829)
- [x] Offline: locate `GNames`
      *2026-07-31: **`GNames` TArray at RVA `0xF9DFEC`** (Data/Num/Max), name hash table at
      `0xF58BF8` with 4096 buckets, and `FNameEntry` decoded: `+0x8` = `(index<<1)|isWide`,
      `+0xC` = chain, `+0x10` = text. **Text is ASCII by default here, not UTF-16 as on BS1** -
      a naive port of `fname_text()` would read garbage. Also fell out: `GNatives` (bytecode
      dispatch) at `0xF6DCB0`, `GMalloc` at `0xF71CC8`, `FFrame` `+0x14` Object / `+0x18` Code,
      `UObject::Class` at `+0x20`, and a constant-time `IsA` via 16-bit interval fields at
      `UClass+0xC0/+0xC2`.*
- [ ] Offline: `GObjObjects` **deprioritised** - `StaticFindObject` uses the object hash, not a
      linear walk, so it did not fall out. BS2's design takes live objects from hook parameters
      rather than scanning, which is cheaper and avoids the class of stall/crash BS1's object
      scanner caused. Revisit only if a use case needs it.
- [x] **Done when:** [ENGINE_NOTES.md](ENGINE_NOTES.md) records the verified build fingerprint, the
      live renderer, and a **working cheat and test-loadout path with the exact command used** -
      including any surface that turned out inert.
      ***I0 CLOSED 2026-07-31.** Build fingerprint recorded; renderer confirmed D3D11 live; the
      cheat path is recorded as a **negative** (every key bind inert, corroborated by the absent
      screenshot) together with the reflection route that replaces it
      (`ConsoleCommand` `0x136070`, `AXPawn::SetWeapon` `0x4F9ED0`, `AXWeapon::AddAmmo` `0x5017D0`,
      `AXPawn::AddInvulnerableFlag`), which is **I2 work** because it needs the adapter. Only
      deferred item is the UELib workspace, held until a specific script question needs it.*

## I1 - Skeleton: inject, log, overlay, command seam (~1 session)

Goal: our code runs inside `BioShockInfinite.exe`, and we can talk to it - before any engine hook
exists.

- [x] `src/game/bioshockinf/` adapter; `HostGame::Infinite` in the registry; data subdir `bsi`
      *2026-07-31 session 35: `bvr::bsi::BioshockInfAdapter`, capabilities 0x0 by design (a bit is
      earned by a hook observed firing, never by an address being derived).*
- [x] CMake file list updated (one DLL, all adapters, runtime dispatch by host exe name)
- [x] Host build fingerprint gate wired to the I0 values
      *All four fields matched live: pe-timestamp `0x627BE455`, size-of-image `0x0124F000`,
      checksum `0x011590C3` (Infinite's exe carries a real one, unlike BS1's), 18,368,840 bytes.
      `buildgate off|on|status` exercises the stand-down path without a sabotaged DLL.*
- [x] **Move the command-file poller into core, ticked from Present.** On BS1 and BS2 it lives in
      the adapter and ticks off an engine hook, so a skeleton adapter has no command surface at all
      until its first hook fires. That made the early sessions on both games materially harder.
      This also moves the ~70 lines of core-owned command vocabulary (`memscan`, `dumpframe`,
      `vrinput`, `vrpace`, `vrmirror`, `vrcine`, `vroverlay`, `vrhud`) that each adapter currently
      re-forwards by hand.
      *2026-07-31: `core/framework/command`, ticked from the Present detour, **opt-in** per adapter
      so BS1/BS2 keep their own pollers untouched (user directive - a parallel BS2 session was live
      in the same file). The core vocabulary is the canonical copy; **folding BS1 and BS2 into it is
      deferred to a consolidation pass**. Also fixed a real trap: a pre-existing `command.txt` is
      now skipped at startup rather than executed.*
- [x] `-Game bsi` through the harness scripts, plus the BS2 conflict guard
      *Landed session 34; exercised end to end this session (cmd/shot/img-diff against the modded
      process).*
- [x] Verify d3d11/dxgi being dynamically loaded does not break `framework::init()` ordering
      *It does not, and the reason is structural: `bioshockvr.dll` links `d3d11` itself, so OUR
      import table loads it before any of our code runs and the throwaway device pulls DXGI in
      turn. Hooks installed T+0.4 s, first Present T+8.5 s, no retry needed.*
- [x] **Done when:** the game launches with both DLLs, the log shows the init chain plus D3D11
      device and swapchain info, F10 toggles the overlay, the game is otherwise normal, and
      `game-cmd.ps1 -Game bsi <cmd>` dispatches **with no engine hook installed**.
      ***I1 CLOSED 2026-07-31.** Backbuffer 2560x1440 `R8G8B8A8_UNORM` windowed, feature level
      11_0, RTX 4060; frame inspector 15/15 context slots. F10 measured at 5.7 % channels changed
      vs a 0.54 % ambient floor; `vroverlay on|off` through the seam at 4.6 %. `dumpframe` wrote
      482 events / 68 resources. Orderly `DLL_PROCESS_DETACH` on a menu quit. Full checklist in
      [TESTING.md](TESTING.md).*

## I2 - De-risk battery (~2 sessions)

Goal: retire the UE3-specific unknowns before building on them. **Every DR gets an ENGINE_NOTES
entry whether it passes or fails** - a recorded negative is worth as much as a positive, and BS1's
dead-end list saved real time.

- [x] **DR-I1 UE3 reflection.** Locate `GNames`, `GObjObjects`, `UObject::ProcessEvent`,
      `UObject::FindFunctionChecked`. Re-derive the `FNameEntry` layout (UE3 differs from UE2.5:
      ANSI/wide union). **Run the static caller census before hooking anything** - the check that
      cracked BS2 after a hook was installed that never fired.
      *2026-07-31 session 36: **PASS, entirely offline.** `UObject::ProcessEvent` `0xCFE70` (vtable
      slot `+0x7C`, thiscall, 3 args, `ret 0xC`), `AActor::ProcessEvent` `0x19A150`,
      `UObject::FindFunctionChecked` `0xD1090` (426 callers), `UObject::FindFunction` slot `+0x54`.
      Two independent routes agree and every census prediction held. The census tool is committed as
      `tools/pe-xref.ps1`. UE3 `fname_text` written (reads the ASCII/wide flag, self-validates on
      the entry's own index). `GObjObjects` stays deprioritised by design. Also corrected three
      ENGINE_NOTES rows: `ConsoleCommand`/`SetWeapon`/`AddAmmo` are exec thunks, not
      implementations. **Live confirmation of the GNames readers is owed** - they cannot be called
      before the engine's static initializers have run.*
- [x] **DR-I2 The camera seam.** `GetPlayerViewPoint` / `CalcCamera` / `Camera.UpdateViewTarget`,
      hooked BS2-style: filter ProcessEvent by cached FName index, mutate the param block after
      calling the original. Confirm it fires in gameplay *and* at the menu, and measure the
      dispatch rate (BS2 sees ~850/s in gameplay, spiking to ~4500/s on load - the fast path must
      stay tiny).
      *2026-07-31 session 36: **PASS, confirmed live.** Fires at the intro AND in gameplay.
      **9681 calls/s** peak (BS2 ~850), ~3600/s idle gameplay; 4.07 M lifetime calls with
      **0 foreign-thread dispatches**. `vrcmd` reports `pump=game` - the handover works. **Game and
      render threads are SEPARATE** (tid 13120 vs 1992), which is free DR-I5 evidence. Path census
      100 % path 2, promoting `[this+0x240]` from inferred to observed. Motion test passes: 12
      positions, and one 360 turn swept yaw **-32392..+32640 = 99.2 % of a full 16-bit range**,
      falsifying the field ordering and the 65536-units-per-turn assumption together. **FRotator is
      SIGNED.** The pump lease passed its positive control in both directions on demand. Still
      open: the returned-view TRANSFORM question - the heartbeat compared against `+0x24C`, which
      is path 1's source, so it said nothing about path 2.*
- [x] **DR-I3 Frame map.** `dumpframe` / `dumpframe full`. Infinite is a **deferred** renderer, so
      the BS1/BS2 forward fingerprints do not apply. Find the tonemap target, the scene RTs, and
      the cb0 screen-ray block offset (BS1 = 12, BS2 = 16, here unknown) using
      `decode-framedump.ps1 -ScanLayout` and `-Diff` across two FOV values.
      *2026-07-31 session 36: **frame map DONE offline** from the banked lite dump - full deferred
      pass order, scene RTs, and the tonemap target (T9, which must be identified POSITIONALLY
      because T9 is reused as the G-buffer albedo and no descriptor rule can separate them), plus a
      free Scaleform half-answer for DR-I7. **The lens offset is still UNKNOWN**, and the old
      instrument could not have found it: core's live watch had already run 19,602 presents with no
      hit, its `>= 320` byte gate excludes the 160-byte tier Infinite's deferred lighting pass uses,
      and `dumpframe full` under-samples an engine that uploads via `UpdateSubresource`. Built
      `dumpframe cb` (mode 3) and `-ScanMatrix`/`-BlockBytes`/`-DiffAspects`/`-SelfTest`; all
      controls pass, including recovering BS2's lens from a dump where `-ScanLayout` finds nothing.
      **THE LENS IS FOUND AND CONFIRMED**: float 0 of the 80-byte constant buffer, row-major 4x4,
      `tanH=|c3|/|c0|` so the object scale cancels. Of 139 candidate 4x4s only one matched the
      backbuffer aspect (the top four by block count are degenerate `tanH==tanV` pairs, one with
      MORE support than the truth - plurality is not evidence). Promoted from candidate to fact by
      a falsifiable prediction: the FOV slider min-to-max moved both axes by the SAME ratio
      (1.14282 / 1.14269) with the aspect held to 0.002 %, 75.01 -> 82.50 deg horizontal. Recorded
      as `kLensFloatIndex` in patterns.h and deliberately NOT wired to core's Vengeance-shaped
      `decode_ray_block`. **And the ini lied**: `FOVAngle=70` + 15 % predicts 70-80.5 deg and a
      1.2094 ratio; the frustum says 75.01-82.50 and 1.1428. I5 must use the frustum.
      Still owed: the aspect cross-check at a second backbuffer size.*
- [x] **DR-I4 Native stereo - TIMEBOXED to one session slice.** Is there any usable engine-side
      per-eye render path? Current evidence says no: `[Stereoscopic3D]` exists in the ini but no
      `bStereo` / `EyeSeparation` / `StereoDevice` names appear in the exe, which points at
      driver-side 3D Vision. If it exists it deletes I6 entirely. If it does not, **write the
      negative down with the evidence and move on** - do not let this become a rabbit hole.
      *2026-08-05 session 37: **NEGATIVE, confirmed live and closed.** GNames pool (69,719 live
      entries): Stereo/Stereoscopic3D/EyeSeparation/StereoDevice/bStereoEnabled all absent;
      `AllowNvidiaStereo3d` present at 4154 (the positive control, and driver-side-only). I6
      builds our own stereo.*
- [x] **DR-I5 Render substrate.** Is command submission a ring (BS2: doubling is free) or a
      kick-and-wait handshake (BS1: needed structural single-threading and cost four sessions)?
      Test `OneFrameThreadLag=False` and `bSmoothFrameRate=False` as the **config-level** lever -
      if that works it buys BS1's `reentry 1t` without a flush-point hook.
      *2026-08-05 session 37: game and render threads SEPARATE under both lever positions;
      `OneFrameThreadLag=False` is accepted and the game plays normally (`bSmoothFrameRate` ships
      FALSE already). Substrate evidence (per-draw UpdateSubresource uploads at 90 M lifetime
      calls, no stall at 9681 camera calls/s) points at buffered/ring submission, NOT BS1's
      handshake. Whether the lag lever shortens camera-to-present latency needs the I6 latency
      instrument - deferred INTO I6 by design, not owed before it.*
- [x] **DR-I6 Exec seam.** Confirm `set <class> <prop> <value>` actually lands. **Verify by effect,
      not by return value** - set the value absurdly rather than to the target. Confirm which of
      the shipped debug binds work by code as well as by key.
      *2026-08-05 session 37: **the seam is PASS by effect** - `bsicall`/`bsiexec` dispatch by
      name through FindFunction(+0x54)+ProcessEvent(+0x7C); `bsiexec setres` RESIZED THE
      BACKBUFFER live (ResizeBuffers hr=0, 20 ms), `bsiexec shot` created its ScreenShots dir at
      the dispatch timestamp. `set ... FOVAngle 130` produced no frustum effect - but FOVAngle is
      independently disconnected from the frustum (the ini lies), so mechanism-vs-dead-property
      stays unseparated; retest on a live property only if I5 ever needs `set`.*
- [x] **DR-I7 Scaleform HUD fingerprint.** How do GFx draws appear in the frame? Does a movie
      render to its own target we can capture directly, rather than being classified out of a
      batch? (That would be materially cleaner than the gameswf classifier.)
      *2026-08-05 session 37: **CONFIRMED live in a second scene at a second resolution.**
      Contiguous end-of-frame draw run on the BACKBUFFER after the scene blit, exactly two call
      sites (0x492284 DrawIndexed / 0x4920FF Draw), BC3 atlases + an R8 glyph atlas. No offscreen
      movie target - but none is needed: the tonemap target (T9 one frame, T8 another - the index
      varies, so the rule is POSITIONAL: srv0 of the last full-screen a=6 DrawIndexed into the
      backbuffer) is HUD-free by construction. The eye image needs NO classifier.*
- [x] **DR-I8 Resolution lever.** `setres` exec vs
      `My Games\BioShock Infinite\XGame\Config\` `ResX`/`ResY`. On BS1 `SETRES` faulted through the
      viewport Exec seam; do not assume either way. **Acceptance is the backbuffer at first Present
      after a relaunch**, never the config read-back.
      *2026-08-05 session 37: **PASS on BOTH levers.** Config: `XEngine.ini` ResX/ResY is a
      boot-derived COPY (a write there is discarded - measured); the real store is
      `XUserOptions.ini` `ResolutionX/ResolutionY`, honoured at first Present
      (`backbuffer 1600x1200`). Exec: `setres` works LIVE via the by-name console seam - no BS1
      fault. Infinite has both lanes.*
- [x] **Done when:** every DR is recorded pass or fail, with its derivation method.
      *2026-08-05 session 37: all eight recorded - **I2 CLOSED**. The session-36 transform
      leftover also closed: GetPlayerViewPoint returns a RAW COPY of the camera POV on the
      observed path (path 2, 100 % census), so I4 injects at the POV/out-params with no
      downstream transform. The FOV law closed at two aspects: VERTICAL-referenced (tanV pinned
      0.4933 at slider max across 16:9 and 4:3; tanH = tanV x aspect exactly).*

## I3 - Headset bring-up: mono big screen (~1 session)

Goal: the game on a giant head-tracked screen. Core does essentially all of this.

- [x] OpenXR session on the game's **own** `ID3D11Device` (zero-copy submits)
      *2026-08-05 session 38: PASS on the simulated Quest 3, zero core/adapter changes -
      session on the game's device first try, backbuffer fmt 28 -> swapchain pair fmt 29 (same
      typeless family, CopyResource zero-copy), FOCUSED in ~600 ms. Survives live
      `bsiexec setres` backbuffer resizes both directions (queued rebuild lane).*
- [x] Quad "cinema screen" layer; pace thread; teardown and alt-tab survival
      *2026-08-05 session 38: sim-verified with numbers - quad world-locked at 1.75 m with
      correct stereo parallax and game pixels; 90.0 fps sustained, `step 5` exact; focus-loss
      holds frame rate and keeps submitting (FOCUSED re-earned); teardown/re-bring-up clean via
      bsivr off/on, hazard waitfail, and resize. The battery's yield was three SIM bugs (clock
      overflow that wedged pacing at ~25 min, sRGB-crushed captures, sticky focus-lose), all
      fixed - see ENGINE_NOTES session 38.*
- [ ] **Cross-check both VDXR and SteamVR.** BS1 never did, and shipped with a Steam Link gap that
      only surfaced later.
      *2026-08-05 session 38: VDXR half user-verified in-headset. **SteamVR half DEFERRED by
      user directive** ("no Steam Link; we can test SteamVR later, not needed for the first
      version") - the debt is carried on the I11 release checklist (post-restructure numbering), not silently dropped.*
- [x] **Done when:** the Quest 3 shows the game on a head-tracked screen, under both runtimes.
      *2026-08-05 session 38: **MET AS RE-SCOPED BY THE USER** - Quest 3 via Virtual
      Desktop/VDXR shows the head-tracked big screen, user verdict "looks pretty good, no
      crashes or freezes/hangs"; F10 VR A/B confirmed in the log (session teardown ->
      re-bring-up live on VDXR), alt-tab survived, two boots, clean exits. SteamVR runtime
      deferred to the release milestone (now I11) per the user (no Steam Link hardware).*

## I4 - 6DoF head camera, and the flat harness (~1.5 sessions)

Goal: a real VR camera, and the ability to test it **without a headset**.

- [x] Camera driven from the predicted HMD pose, including roll; world scale; recenter
      (s39: out-param substitution in the detour tail, lane order replay->sim->live; every
      axis flat-verified in exact rotator units/UU; worldScale default 50 = UE3 canonical,
      F10 slider + `vrpreset save`; quad stays the submit path - camera mode is never set)
- [x] **Build `simhead` in this milestone.** On BS1 it arrived at session 12 and the docs name it
      as the thing that should have come first: it drives the entire camera path exactly as a
      headset does, flat, and it is what reproduced the head-coupling bug with no headset at all.
      Include the position triple (BS2's version) so 6DoF is provable flat, not just rotation.
      (s39: BS2's 3/4/6/7-arg form, self-expiring, recenters on arming from idle)
- [x] **Build record/replay (`vrrec`) in this milestone too.** On BS1 it arrived at session 20.
      (s39: BVRR v1, present-edge cadence - this seam fires many times per frame - replay
      marks number-for-number identical to record, flat with `xr=none`)
- [x] Camera write discipline: BS1 wrote pitch **absolutely** and yaw **relatively**, which froze
      the engine's own view pitch at -88.9 degrees for a whole session and silently broke melee,
      because nothing else wrote it and nothing noticed. Prefer a **servo through the game's own
      input path** over writing engine memory; if writing directly, make sure something reads back
      and something notices. (s39: out-params ONLY - engine memory never written, so the engine's
      view stays engine-owned and kept moving under the drive, observed live; heartbeat prints
      engineRot + pitchErr every beat so divergence is a number; pitch servo deferred to I7 -
      publishing the error would arm core's pitch kill and seize right-stick Y)
- [x] 1 Hz heartbeat logging the FINAL camera (post drive and offsets) so flat checks measure
      numbers directly (s39: the `[bsi] drive:` line - lane, final loc/rot, engineRot,
      pitchErr, headOff, scale, xr state; 10-beat self-expiry kept)
- [x] **Done when:** lean physically around a corner in Columbia with no drift and correct roll,
      user-verified in the headset; and `simhead` reproduces a head-driven camera flat.
      (s39: BOTH halves done - flat battery green end to end, and the user's VDXR verdict:
      lean tracked with NO drift, roll correct. World-scale FEEL could not be judged on the
      mono screen - the tune is deferred to I5, where stereo makes it a real judgment; the
      lever, slider and vrpreset persistence are in place. Core's VR-section camera toggle
      is inert on Infinite by design until I5 - the user pressed it and correctly saw
      nothing.)

## I5 - Stereo (~2-4 sessions - the risk milestone, pulled forward per the BS2 lesson)

Goal: true geometric stereo with 6DoF. BS2 reached in-headset-accepted stereo on its THIRD
session, before any lens/resolution polish - and the polish milestones were better for it,
because every judgment finally measured the thing the headset shows. Same bet here.

Ladder, in this order, because it worked on BS1 and each rung is independently shippable:
MonoScreen (done - I3), MonoTracked (I4's camera under the quad), AlternateEye, then
SequentialReentry.

- [x] Entry gate (RESHAPED at the s38 restructure): a trustworthy projection CLAIM - which the
      I2-derived FOV law already provides (vertical-referenced, tanV 0.4317..0.4933,
      tanH = tanV x aspect, verified at two aspects). The FULL multi-lens decoder now lives in
      I6; pull it forward ONLY if stereo shows a lens question the law cannot answer. A wrong
      FOV claim still makes every subjective stereo judgment worthless (BS1's M4) - the gate is
      unchanged in spirit, only its instrument is lighter.
      *2026-08-05 session 40: claim published per detour call from the law (tanV default =
      slider min 0.4317, `bsifov tanv` lever, vrpreset-persisted) + publish_gameplay_view
      (core's cine fallback needs it). Audit src=readback, tanH exact; Infinite's OWN
      claimRatioH baseline derived fresh: **0.5576** at slider-min/16:9 vs the symmetric
      54-deg sim eye. Caveat recorded: no live option reader until I6, so the claim assumes
      the in-game slider at minimum.*
- [x] AlternateEye rung (core already supports it) as the de-risking step
      *2026-08-05 session 40: vraer one-toggle; apply_eye_offset on the full rotation basis
      (inf-local ue_rot_basis). Flat: inter-eye |d| = ipd x scale exact (3.150 UU at
      63 mm/50), doubles at scale 100, both signs observed.*
- [x] SequentialReentry: find the scene-build root and call it twice per game tick with per-eye
      cameras. Whether a single-threading rung is needed at all is DR-I5's answer - and DR-I5
      recorded a threaded, ring-buffered substrate (90 M UpdateSubresource, no stalls at 9681
      calls/s), the same shape that let BS2 run SR with NO 1t machinery at all. Test threaded
      first; port nothing from BS1's single-threading kit until a measured stall demands it.
      *2026-08-05 session 40: the root is the VIEWPORT draw `0x1FDE30` (canvas -> client
      draw -> present kick), derived live (census + stack scrape + vtable probes; the
      client-draw-only double is a recorded negative - no second present, ring skew).
      THREADED doubling ran clean: draws/s=90 2nd/s=90 presents/s=180 camReplays/s=90 at the
      sim ceiling, call2 80-215 us, zero faults, 15-min armed soak green. No 1t ported.*
- [x] **Gate pass 2 deny-by-default** on a known gameplay caller return RVA, so doubling can never
      run inside a load path.
      *2026-08-05 session 40: ret `0x206309` (4 static callers exist; census names it the
      per-tick dispatcher), jointly with camera-silent(400ms) + present-stall + teardown +
      poison gates. Observed refusing a foreign caller live (f=2, no double).*
- [x] Pair pacing: one `xrWaitFrame`, one locate, one prediction per game tick. Submitting the two
      eyes of a pair with poses located a compositor period apart produces motion-dependent shear.
      *2026-08-05 session 40: armed in the vrstereo ladder; core logged "pair pacing live"
      on the first eye pair; 90 pairs/s sustained.*
- [x] Camera pass 2 is a **replay of pass 1's cached base**, not a re-sample - a Present lands
      between passes, and re-sampling skews the pair into vertical disparity.
      *2026-08-05 session 40: absolute replay + 100 ms staleness guard; replay BURSTS ==
      second draws exactly; L/R capture pair shows real parallax (mean 0.42 / 1.09 %
      channels) against a byte-identical mono-projection control pair.*
- [x] **Done when:** true geometric parallax (verified in the headset), 72 fps at default render
      scale, a 30-minute session with no visual state corruption, and stability across a level
      transition and a quit-to-menu.
      ***I5 CLOSED AS RE-SCOPED 2026-08-05 (session 40, user verdict on VDXR):** "there's
      stereo 3d rendering and it's working well" - true geometric parallax confirmed;
      nothing broke in their testing; the log measured 77-80 eye pairs/s (155-160
      presents/s) at default scale with every SR gate exact - above the 72 fps target,
      though a slight judder suggests the headset was at a higher refresh (try VD 72 Hz).
      **The "window" percept is the expected honest-claim behaviour**: the game renders
      75 x 47 deg inside a ~108 x 110 deg eye - filling it is I6's whole purpose (FOV
      lever + near-square render; resolution alone cannot help under the
      vertical-referenced law). **Carried to I6's headset session, explicitly:** the
      world-scale tune (slider works, feel unjudgeable through the window), the judder
      verdict, and the 30-minute soak (this session was a couple of minutes by design).*

## I6 - Lens audit, FOV, resolution, and the config menu (~2 sessions, after stereo per BS2)

Goal: **user priority 1.** A correct projection everywhere, a resolution picker, and a real
config/preset UI - now judged IN STEREO, the way BS2 s32-37 did it (its picker, automatic FOV
and viewmodel-lens fixes all landed after stereo and were judged against the headset).

Much of the original milestone is already banked: the FOV law is derived (I2, two aspects), and
resolution has BOTH proven levers (DR-I8: `XUserOptions.ini ResolutionX/Y` honoured at boot;
`bsiexec setres` live, XR swapchain rebuild surviving it - s38). What remains:

- [x] **The lens decoder.** Stride-sampled, majority-voted, structurally validated, multi-lens.
      **Assume the frame carries more than one lens until proven otherwise.** Publish the
      runner-up as a named second lens. Refuse to publish a round without a clear majority
      rather than publishing a confident wrong value. BS1's watch latched onto the viewmodel
      frustum, and that single choice was the release blocker. *(s41: `bsilens` - matrix-law
      decode off an opt-in core cb tap, 60%-of-16 majority, aspect gate load-bearing, runner-up
      named, refused rounds keep last-good. Caught a wrong claim twice in its first hour. No
      second lens visible at the attract - the gameplay-save check rides the headset session.)*
- [x] **Cross-check the lens law at a non-16:9 aspect live** (the I2 derivation already did
      16:9 vs 4:3 offline; keep the discipline for every new lens found). *(s41 at 1440x1440:
      the law's ANCHOR corrected - degrees are horizontal at a FIXED 16:9 reference, tanV =
      tan(deg/2)/(16/9) pinned; both decoders 0.6704 exact, claim delta 0.0% after the fix.)*
- [x] FOV lever. The native slider spans only ~46.7-52.6 deg vFOV, so a lever is needed - the
      property chain is *named*, so try `set`-by-name before any memory scan. *(s41: set-by-name
      tried FIRST and measurably dead (writes nothing); every FOV cache recomputes per tick; the
      lever ENFORCES [cam+0x214]/[cam+0x3D0] per camera dispatch - tanV to 1.2+ proven, exact,
      0 faults, self-restoring disarm. `bsifov set <deg>`, F10 slider, preset-persisted.)*
- [x] Resolution picker UI: named square-first modes plus custom, live-vs-config comparison, and
      the aspect warning (a headset eye is near-square; 16:9 throws away most of its width).
      *(s41: flat/squareperf/eye/native/sharp + custom, 1 Hz ini-vs-live, aspect warning,
      recommends-annotation, live setres + XUserOptions.ini persist in one Apply, `bsires`.)*
- [x] **Call `xrEnumerateViewConfigurationViews`.** Neither BS1 nor BS2 ever does, and the docs
      name its absence as the missing input for both a correct FOV policy and a derived render
      target (`recommendedImageRect`). *(s41: at bring-up, additive, `recommended_eye_size()`;
      BS1 full sim lane re-run green as the inertness proof - claimRatioH 1.01769 unchanged.)*
- [x] ~~**Extract `bvr::config` into core**~~ **ADAPTER-LOCAL by decision** (ARCHITECTURE
      decision log 2026-08-05 s41: the decoupling directive outranks the third-consumer trigger;
      core extraction deferred to the healing session). Per-key registration and the config menu
      with **named preset save and load** are DONE adapter-locally (`bioshockinf/config.cpp`:
      KeyDesc registry, vrpreset.ini legacy-compatible, `bsi\presets\<name>.ini`, F10 slots,
      `vrpreset saveas/load/list`).
- [x] **Done when:** a full preset round-trips through the menu across a restart *(s41: 6/6 keys
      both directions)*; the resolution picker's value is confirmed by the backbuffer at first
      Present *(s41: `first Present: backbuffer 1440x1440` after the mod's own ini write)*; and
      the claimed projection matches the rendered frustum **at a non-16:9 aspect**, measured,
      not argued *(s41: 0.6704 both instruments, claim delta 0.0%, claimRatioH 0.48705 vs
      0.4871 predicted)*. **Flat half CLOSED s41. Headset half (same day): the FILLED-EYE
      verdict is GREEN at lever 132 ("no space warp - perfect"), world scale tuned to 150,
      preset redesigned to ONE Save/Load carrying the whole session (verified auto-restoring
      at boot). s42: the judder FLAT half is DONE - pair-cadence jitter instrument (TRACE
      pairs line: interval mean/sd/min/max + the waitGate share) + the opt-in
      `vrpace sync` pair-rate cap (default off in core, armed with Infinite stereo, F10
      A/B checkbox; BS1 sim-lane inertness proof in the commit). Measured: the SIM's
      xrWaitFrame gates strictly (pairs lock to 72/90 = refresh, waitGate 540-620 ms/s),
      so the free-run beat is a PIPELINING-runtime behaviour the sim cannot reproduce -
      whether VDXR free-runs is answered by reading pacetrace.log after the next headset
      run, and the sync checkbox is the fix either way. The 30-min soak is DEFERRED by
      user decision (2026-08-05) to a later/release soak. **HEADSET VERDICT IN
      (2026-08-06, VD at 80 Hz)**: pacing LOCKED AND CLEAN in steady seconds (pairs ==
      refresh, sd 0.3-1 ms) - the beat hypothesis is falsified on VDXR too; the judder is
      recurring engine/GPU HITCH SPIKES (39-113 ms pair intervals in bursts), worse
      outdoors at native res, better at the `eye` preset - a render-load/streaming
      matter, not a pacing defect. The hitch-source hunt (texture streaming vs GC vs
      shaders vs encoder) and the user's "camera slightly jumpy" observation carry
      forward as their own item; the lens/FOV/resolution/config milestone itself is
      complete. *(s43, 2026-08-06: THE HUNT RAN - spike instrument + mid-stall stack
      sampler in core (opt-in, BS1-proofed), flat repro via the walking pad lane at
      native 2064x2208, and the dominant source NAMED BY A-B-A: the engine's 30 s GC
      tick (TimeBetweenPurgingPendingKillObjects=30 -> 300 removed the idle spike
      grid and took the matched wander from 4-7 spikes to 0; reversal returned it).
      Candidate fix live in the game folder's DefaultEngine.ini (backup beside it);
      headset verdict pending - S43 checklist in TESTING.md. Streaming/traversal
      residual documented with the texture-pool lane researched next; the "jumpy
      camera" observation stays open - not instrumented separately this session,
      the spike correlation instrument now exists for it.)*
      Standing I8 guard from the user: the lever must not break the viewmodels -
      **ANSWERED 2026-08-06 on the user's pistol save, weapon and hands DRAWN**: lens1 ==
      the lever claim exactly (tanV 1.2634, 100% of 211 valid samples, delta 0.0%), NO
      second lens (0% support). Infinite renders the viewmodel through the SAME
      projection as the world - no separate fg lens exists to break, and no BS1/BS2
      fg-FOV counter-modeling is needed. (Honest caveat: the decoder names a runner-up
      only at >=10% support; 100%-of-valid across weapon-drawn rounds is the strongest
      evidence this instrument can produce.)**

## I7 - Motion controllers and decoupled aim (~2-3 sessions, one lane per BS2 s39-40)

Goal: **user priorities 3 and 4.** The game fully playable from the headset, every action bound,
and the controller aims while the head looks. BS2 did the whole M6/M7-parity block fresh in two
sessions once stereo and camera were real; controllers without aim are half a feature, so they
ship as one lane here.

Controls half:

- [x] OpenXR action set, Quest 3 Touch bindings, synthetic-XInput lane *(s42: LIVE flat -
      `bsiinput on` verifies + re-points the ord-2 IAT slot (0xCD4814, target read in
      XINPUT1_3.dll), core bridge composes, the game polls through the wrapper (iat 5642),
      sim right-stick TURNED the camera (yaw 65->145->-133 deg, shot diff 52% pixels), sim A
      pressed. No UpdateInput pump / SetUseController - Infinite polls XInput itself; BS2's
      activation machinery correctly does not port. Movement was scene-locked in the probe
      save - re-test walking in free roam.)*
- [x] **Extract the XR-to-pad mapping table out of core** into a per-game table the adapter
      supplies. *(s44 DONE: `PadProfile` enum + two `constexpr PadMap` tables. One atomic in
      the bridge, default Bioshock1, one relaxed load per compose; the BS1 table reproduces
      the old literals exactly and BS1/BS2 never call the setter. BS1 inertness PROVEN on the
      sim lane per control, not asserted - faces, grip hysteresis boundaries, RS-click still
      eaten, flick still three-way, turn suppression by an A-B pair, claimRatioH 1.01769 ==
      the banked 1.018, zero faults. Infinite's map measured the same way: faces straight
      through, RS-click FORWARDED (0x0080), the new fourth flick direction (0x0008 DR), stick
      polarity. Seam choice and the rejected bsi-local duplicate: ARCHITECTURE decision log.)*
- [x] **Controls arm at boot** (`inputOn`, a 9th preset key, default on) - a headset boot came
      up with no controller and no way to fix it, since every in-headset judgment must be an
      F10 control and reaching F10 needs a controller. *(s44)*
- [x] Infinite-specific mapping work that BioShock has no analogue for. *(s44: all of it rides
      the game's OWN bindings through the pad, so it is a mapping question, not new code -
      two-weapon carry on the bumpers, vigors on LT, sprint on LS-click. The DPad family
      (nav pulse / buyout hack / auto hack / quick-toggle cycle) has no Touch analogue and,
      per the user's call, reuses the thumbrest+flick lane with a fourth direction added;
      menu nav rides the left stick, which this game serves natively via
      AxisEmulationDefinitions; test-only cheats stay on the keyboard. Sky-Hook melee nuance
      and item toss are left for the model-sync session - nothing invented.)*
- [x] **The pause menu works from the controller** *(s44: flagged as a blocker by the flat lane,
      then FALSIFIED in the headset the same night - "the menu and exiting the menu is working
      as expected from the controllers". The flat null was a harness focus artifact: the menu
      parks the game thread and `xrsim-cmd` does not foreground, so the presses landed on an
      auto-paused window. Nothing to port from BS2.)*
- [x] **The Skyline (TBar) control family.** The shipped ini documents an `XInputHandler` chain
      mechanism where commands joined by `+` short-circuit on success, with a large TBar set
      (transfers, boost, dodge, melee transfer, zoom, and the highlight-effect pairs). Any VR remap
      must preserve that chain semantics. *(s42: fully audited into ENGINE_NOTES; the synthetic
      pad drives the game's OWN bindings through the IAT, so the chains are preserved by
      construction - a remap only ever changes which pad control the XR input lands on.)*
- [ ] Drive the named UE3 axes (`aLeftStickX` etc.) directly - a large advantage over BS1, where
      `exec NextWeapon` faulted and weapon switching could not be driven flat at all. *(s42:
      probably unnecessary - the pad axes drive the same bindings and `bsicall NextWeapon`
      dispatches as a PC UFunction; keep only if a VR remap needs an axis the pad lacks.)*
- [x] Expect the Steam overlay to eat `XInputGetState` at the proxy thunk; the IAT-hijack lane is
      already scoped (slot RVA `0xCD4814`). *(s42: patterns.h `kXInputGetStateIatRva`, verified
      live before the re-point - under the sim launch the slot still pointed at XINPUT1_3.dll;
      the overlay case rides the first Steam-launched headset run, where the wrapper keeps
      whatever chain the slot holds.)*

Aim half:

- [x] Find UE3's fire path; substitute at the origin-and-direction seam so per-weapon spread still
      applies downstream. *(s44 DONE, HEADSET-VERIFIED: `APawn::GetBaseAimRotation`, a virtual at
      pawn vtable +0x2E8, `FRotator* __thiscall (FRotator*)`, `ret 4`, impl read live off the
      pawn. User: "aiming is not influenced by the head - the bullet kept going in the same
      direction as my controller". The rotation is substituted, so per-weapon spread still
      applies downstream by construction. Ships ARMED.)*
- [x] **Both hands, decoupled** *(s44b: trigger attribution, latched - right trigger = weapon
      hand, left = vigor hand. `aiming hand` is live in the F10 panel.)*
- [x] **Aim laser and aim dot, per hand, toggleable; dots on by default** *(s44b: core's existing
      two-slot API, no core change. Measured with the hands at OPPOSITE angles - both
      `aimRayMaxDevDegL/R` read 0.0000, so each dot sits exactly on its own hand's ray, which
      proves the round-trip math and the per-hand attribution in one measurement.)*
- [x] **The head-coupled-aim defect does not exist** - falsified by measurement, s44. With the
      hand parked and only the head moving, the engine's aim tracks the head degree for degree
      (+40 -> -130, -40 -> -50, back to 0 -> -90). The aim chain is downstream of the camera
      drive, so shots already land where you LOOK; the open half is aim following the
      CONTROLLER.
- [ ] **The FRotator trap**: rotation units are int32s whose float reinterpretation is a denormal,
      so a live fire direction prints as `(0.000 0.000 0.000)`. This cost BS1 a long detour.
      Classify out-params by value, not by position.
- [x] Read-only probe hooks (install both, refuse to substitute) so a diagnostic **cannot change
      what it measures**. BS1: "any future 'is this seam involved at all' question should start
      there." *(s44: `bsiaim probe on` installs read-only and refuses to substitute; `bsiaim on`
      is the separate explicit write. It is what falsified the head-coupling assumption.)*
- [x] Respect `ret imm / 4` for every probe (see the RTC warning in ENGINE_NOTES). *(s44: the
      aim install REFUSES unless it can see `C2 04 00` in the target body, the stricter of the
      two patterns in the tree.)*
- [ ] Per-weapon aim profiles keyed by class name, covering the DLC weapons
- [ ] **Done when:** fully playable from the headset with every action bound (audited against the
      shipped `DefaultInput.ini` command list), and look one way / shoot another - impacts land
      where the controller points, user-verified.

## I8 - Viewmodel and hands decoupled from the headset, and scale (~3 sessions)

Goal: **user priority 2.** Hands, weapons and Vigors stuck in space, not glued to the head, at a
believable size.

This was the single largest sink on BS1 (about seven sessions plus a relapse). **Apply the policy
gate before writing a line of compensation code.** The reference implementation SHAPE is BS2
sessions 40-41 (hands split per-controller, the holdable lane, and the animation-preserving
drive whose written-quat/anim-quat discipline is documented in VERIFICATION 2.8) - derive fresh,
port nothing.

- [x] **Handoff from s44 (I7):** the user expected the weapon model to stop riding the headset as
      part of the controls block. It is not - that is this milestone, and I7 deliberately left it
      alone. What I7 DID leave here: the controller ray is already built and published on the
      view's own basis (`aim.cpp` `controller_ray`, game yaw + recenter residual), so the model
      drive can consume the same ray the aim seam does rather than deriving a second one that
      drifts. The flat sweep also showed the viewmodel moving with the sim's hand pose (2.6%
      frame diff at +/-35 deg), which is a starting observation for what already tracks what.
      *(s45b: taken up - `frame_context.h` publishes ONE basis from drive_view and both the ray
      and the model consume it; 0.0000 deg deviation at five stations incl. rolled.)*
- [x] Step 1: check what UE3 does natively here. *(s45b: the FP rig is an XFirstPersonAttachment
      ACTOR reached by the pawn's own `GetFirstPersonAttachment` native; its
      XSkeletalMeshComponent at +0x218 renders hands AND weapon in one 43-bone skeleton
      (PlayerHands*/L_Grip/R_Grip), animated by Morpheme networks; the weapon hangs off the grip
      subtree, so driving grips carries the holdable for free. Full trail in ENGINE_NOTES.)*
- [x] Step 2: **test whether the BS1/BS2 defect even exists.** *(s45b: the lens half was already
      settled - no fg lens, s41/s44. The head-riding itself is structural: the attachment's
      LocalToWorld follows the camera, so SpaceBases writes composed through the live L2W
      INVERSE cancel it exactly - no counter-modeling, no render-side patches. Poke oracles
      falsified BS2's "engine never restamps scale" on this build - adoption takes whole atoms
      and release is stop-writing.)*
- [x] Step 3: only then port compensation machinery, and only the parts proven necessary.
      *(s45b: ported IN SHAPE - per-hand parallel arrays, adopt-then-compose, qtc = conj(qa) x qt
      preserving authored frames, arms game/follow/hide with the collapse rule, pass-2 verbatim
      reapply. NOT ported, each with its measured reason: scale-row pinning (engine restamps
      scale here), explicit release-restore (engine self-heals), the wskel weapon lane (no part
      inverse-scales - the pistol scales WITH the hand), pitch kill (stick-Y measured: model
      bit-identical under full stick-up).)*
- [x] UE3 differences that matter: the skeleton is `USkeletalMeshComponent::SpaceBases`, not a
      Havok `hkQsTransform` array; attachment is components and sockets, not the UE2 Base/Owner
      pair. BS1's bone drive transfers in **shape only**. *(s45b: confirmed and derived -
      32-byte FBoneAtom {quat, trans, uniform scale}, SpaceBases +0x290, LocalAtoms +0x29C,
      RefSkeleton mesh+0x74 stride 0x50 and NAME-FLAT (parents all 0; names carry structure).)*
- [x] The principle that does transfer: **engine-side writes are re-read by the engine, so attached
      objects and effects follow for free; render-side matrix patches leave separate objects
      behind** - the Vigor hand FX above all. *(s45b: the drive is engine-side SpaceBases writes;
      the pistol followed the driven grip with no separate handling from the first frame.)*
- [ ] World scale calibration, and the world/viewmodel scale split if it proves necessary
      *(s47 groundwork, flat: ground truth re-verified exact - 1.000 m commanded -> +150.0 UU
      written, single-axis, zero cross-axis at worldScale 150 - and the cm->UU audit table in
      ENGINE_NOTES shows every adapter conversion riding the live `fc.worldScale` (the dot's
      cm->XR-meters is correctly scale-free). The calibration itself is a headset judgment;
      box stays open.)*
- [x] **The I8 remainder, worked s47 (flat-green, headset pending)**: boot-time glue arming
      measured IMPOSSIBLE (the boot/checkpoint pose IS the stance - 101.11 deg at the anchor,
      stable from resolve; capturing it would invert the glue - first-shot arming stays); the
      reapply-burst gate closed measured-no-defect (33,255 replays, skippedStale=0, maxAge
      63 ms - the edge-triggered release, not the timer, is the protection); ANIMTRANS built
      from evidence (reload travel R 14 cm / L 48 cm measured with the new `bsibones travel`
      peak tracker, pass-through exact to 1 UU in the driven A/B; default OFF, unpersisted);
      the per-weapon profile SCAFFOLD (class-name key via the new `reflect::class_name_of`,
      zero entries - values wait for the I9 arsenal save; the fire seam's Weapon param is
      NULL on ordinary shots, so the pawn-side source is I9 derivation).
- [ ] **Test from the START of the game, not only from a loaded-out save** (user directive,
      2026-07-31). A reflection-conjured arsenal is convenient but unrepresentative: the opening
      hours have no weapon at all, then the Sky-Hook alone, then one gun. Calibration that only
      holds with a full loadout is not calibration. The user will produce the combat/weapons save
      at this milestone rather than earlier, precisely so both ends get covered.
- [x] **The four s45b headset findings, worked s46 (flat-green, headset pending)**: the
      persistent stance = the SubtleFidget lane, killed by the ready-pose glue (A-B-A
      0.33 -> 101.11 -> 0.06 deg, default ON); the fire ORIGIN seam
      (AXPawn::XGetWeaponStartTraceLocation reads the camera's GetPlayerViewPoint -
      substituted to the aiming hand, ships armed, 77.4 UU measured parallax); wrist-cap
      hide styles 0/2 (style 1 rejected flat - skin hood); the per-hand arm-relative
      wrist quat, built inert. TESTING.md "S46 checklist" is the verdict list.
- [ ] **Done when:** the weapon is one with the right controller and the Vigor hand one with the
      left, inspectable from any angle, believable size, effects attached, and **they do not move
      with the head** - user-verified in the headset.

## I9 - Presentation: HUD, menus, effects and cinematics (~2-3 sessions, one lane per BS2 s42)

Goal: **user priorities 5 and 6.** Readable HUD, usable menus, correct full-screen effects, and
both cinematic classes working in VR. BS2 shipped its whole presentation surface as a single
lane (session 42: HUD panel redirect, cinema verdicts, bars, effects) - the pieces share the
same instruments and the same eye-image rule, so they ship together here too. Note DR-I7's gift:
the Infinite eye image needs NO classifier (the positional rule - srv0 of the last full-screen
a=6 DrawIndexed into the backbuffer - is HUD-free by construction).

**Deferred INTO this milestone by user call (2026-08-10, s51 close-out) - both
playable-with, both fully instrumented, neither blocks the ladder:**

- [ ] **The FOV-edge near-field drift** (hand pulls toward the camera off-center, worst
      left; stationary head). Exonerated so far: projection split (s49), compose chain
      (s50), eye-tag claim (s50). Pick it back up WITH the s51 discriminators - the
      hand-ref quad one-look (`bsicam handquad on`), the VDXR view logger
      (`bsicam viewlog`), and THE EDGE-TELEMETRY RUN (`bsicam edgelog`, TESTING S51
      item 5) - the TSV decides which half of the hypothesis space to follow.
- [ ] **The FX-origin frozen family** (muzzle flash, tracer, vigor charge plume, ready
      sparkle, muzzle smoke - all camera-anchored at the authored hand pose). SIX lanes
      falsified (ENGINE_NOTES s50 + s51 part 3; the decoded record lane is COLD at
      runtime). Next leads: the live 0x3EC4C0 caller 0x5EC393's loop, or a render-side
      particle transform hunt; instruments banked (`bsifx u dump|callers`, `bsifx t`).
      Fallback if the hunt keeps stalling: hide the camera-anchored FP effects
      (the hand-attached riders already read correctly).

Per-weapon presets (pulled to the FRONT of this milestone by user call, s51 close-out):

- [x] **The cheated arsenal** (s52, commit 0e4a41c: `bsigive <Archetype> [ammo]`).
      The route CORRECTED in derivation: identity is the ARCHETYPE (all weapons are
      class XWeapon), and the grant is DynamicLoadObject("PreCoalescedItemAssets.
      <Archetype>") -> pawn AcquireWeapon(archetype) -> manager EquipWeapon(instance)
      -> AddAmmo - the s43 falsification was the CDO-vs-archetype shape. The
      calibration SAVE is the user's headset half. The 2026-07-31 "test from the
      START" rule STANDS for regression coverage.
- [x] Fill the s47 profile scaffold (s52: ARCHETYPE-name key; auto-capture on
      switch-away, apply-on-equip, weapons.ini persistence, F10 WEAPON PROFILES;
      values derived in the headset are the remaining half)
- [x] The vigor hand is a weapon too - per-vigor BY CONSTRUCTION (each vigor is its
      own archetype key; GetEquippedWeapon(1) answers the vigor hand)
- [ ] **Done when:** switching weapons in the headset lands every weapon aligned
      (aim ray on the muzzle, model seated in the hand) with no global-tune
      compromise, values surviving a save/load round trip.

HUD/menus/effects half:

- [x] Scaleform GFx classifier: NOT needed - the positional rule shipped as core
      `gfx_hud.cpp` (s52, a2aea90): boundary = the only full-screen depth-free a=6
      backbuffer DrawIndexed; everything after = the UI run, redirected
- [x] Health / Salts / Shield / ammo / Vigor selection on a readable panel in both eyes
      (s52 sim captures; headset readability judgment pending)
- [x] Pause menu on the panel (s52 capture); upgrade/vending/hacking UI expected on the
      same lane - headset confirm pending
- [x] Full-screen effects (s52): the explosion family is SCENE-SPACE (already full-view);
      the GFx hurt-flash class passes through to the eye image (`bsihud fx`, census-proven)
- [x] Backbuffer-content detectors: nothing consumes the letterbox watch on Infinite; the
      gfx_hud classifier reads the draw stream BEFORE our writers by construction

Cinematics half:

- [x] **Bink FMV**: covered by the silence architecture (s52, e8dc538) - the camera seam
      goes silent, drives stop writing by construction, the stale-publish quad shows the
      movie; gfx_hud classifies nothing without a boundary blit. Headset visual confirm
      on the attract pending.
- [x] **Engine-rendered Matinee**: the s52 detector (GetViewTarget poll) + hold gates -
      hands/aim/laser/fire release edge-clean; headset regression run pending
- [x] These are two different problems - confirmed, and they got different handling
      (silence vs detector)
- [ ] Correct projection claim throughout - BS1's cinematics rendered their own FOV while the claim
      said something else, and read as a fisheye (judge on the headset Matinee run)
- [x] Selectable rig behaviour during cutscenes: the s52 HEAD RADIO (head look default /
      fixed head; `cineHeadLook`, F10) - the "off" third mode deferred until a scene
      demands it
- [ ] Subtitles readable in stereo
- [ ] **Done when:** a non-developer can read their health and ammo and navigate the menus from
      inside the headset, and both cinematic classes play correctly in stereo with no fisheye and
      no black-bar artifacts.

## I10 - Infinite-specific set pieces and DLC tuning (~2-3 sessions)

Goal: the things BioShock never had, plus the full DLC sweep.

- [ ] **The Skyline.** It is a rollercoaster and a comfort problem with no BioShock analogue.
      Expect to need comfort options (vignette, and possibly a rig-stabilisation mode) that the
      other two games did not.
- [ ] Songbird sequences, Handyman fights, Elizabeth's tears
- [ ] Full tuning sweep across **Burial at Sea 1 and 2 and Clash in the Clouds**: aim profiles for
      the DLC weapons and Old Man Winter, scale, HUD placement, viewmodel calibration
- [ ] **Done when:** a Skyline ride is comfortable, and aim, scale, HUD and viewmodel calibration
      hold in all three DLC.

## I11 - Release (~1-2 sessions)

- [ ] **SteamVR runtime cross-check** - the debt deferred from I3 (user, 2026-08-05: no Steam
      Link hardware, not needed for the first version). BS1's Steam Link gap surfaced
      post-release; do not ship without at least one SteamVR-runtime session or an explicit
      release-notes caveat.
- [ ] Packaging, bundled preset (a fresh install should need no tuning), README, release notes
- [ ] Version stamped from `project(... VERSION)` + `git describe`, never a hand-edited define.
      BS1 shipped "0.1.0" across three releases and it cost a whole session of PE-timestamp
      forensics to attribute one external crash report.
- [ ] Desktop mirror pinned to one eye; the game keeps running when the headset comes off
- [ ] **A FULL PLAYTHROUGH IS THE GATE.** BS1 learned this the hard way: in one session every fix
      was accepted on a single check, and **two of the three regressions were found by the user
      playing, not by any check that was run.**
- [ ] Merge `bioshock-infinite` into `main`

---

## Deferred / post-v1

- Hand IK arms (opt-in; a slightly-wrong IK arm reads worse than floating hands and a weapon)
- Two-handed weapon handling
- Left-handed mode
- Seated / standing recenter modes
- Wrist-anchored HUD
- Asymmetric per-eye projection matrices
- Selection wheels
- OpenVR backend
