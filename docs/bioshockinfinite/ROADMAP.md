# Roadmap - BioShock Infinite

Milestones for the BioShock Infinite (UE3) adapter. Separate ladder from the BS1/BS2 M0-M10 in
[../ROADMAP.md](../ROADMAP.md), prefixed `I` so the two never collide.

Ordered **by engineering dependency**, not by desirability. The user's stated priorities appear as
acceptance criteria, not as ordering: on BS1 the resolution/FOV question could not be judged at all
until stereo ran, and two sessions were lost to mono screenshots that measured the wrong thing.

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
- [ ] **DR-I2 The camera seam.** `GetPlayerViewPoint` / `CalcCamera` / `Camera.UpdateViewTarget`,
      hooked BS2-style: filter ProcessEvent by cached FName index, mutate the param block after
      calling the original. Confirm it fires in gameplay *and* at the menu, and measure the
      dispatch rate (BS2 sees ~850/s in gameplay, spiking to ~4500/s on load - the fast path must
      stay tiny).
      *2026-07-31 session 36: read-only detour WRITTEN and building, gated on the live prologue AND
      on finding `ret 8` in the body before the hook is created. Takes over the command poll on
      first fire. **NOT yet observed firing** - BioShock 2 held the machine, and a hook is not real
      until it dispatches. Checklist in TESTING.md.*
- [ ] **DR-I3 Frame map.** `dumpframe` / `dumpframe full`. Infinite is a **deferred** renderer, so
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
      Needs one live gameplay session.*
- [ ] **DR-I4 Native stereo - TIMEBOXED to one session slice.** Is there any usable engine-side
      per-eye render path? Current evidence says no: `[Stereoscopic3D]` exists in the ini but no
      `bStereo` / `EyeSeparation` / `StereoDevice` names appear in the exe, which points at
      driver-side 3D Vision. If it exists it deletes I6 entirely. If it does not, **write the
      negative down with the evidence and move on** - do not let this become a rabbit hole.
- [ ] **DR-I5 Render substrate.** Is command submission a ring (BS2: doubling is free) or a
      kick-and-wait handshake (BS1: needed structural single-threading and cost four sessions)?
      Test `OneFrameThreadLag=False` and `bSmoothFrameRate=False` as the **config-level** lever -
      if that works it buys BS1's `reentry 1t` without a flush-point hook.
- [ ] **DR-I6 Exec seam.** Confirm `set <class> <prop> <value>` actually lands. **Verify by effect,
      not by return value** - set the value absurdly rather than to the target. Confirm which of
      the shipped debug binds work by code as well as by key.
- [ ] **DR-I7 Scaleform HUD fingerprint.** How do GFx draws appear in the frame? Does a movie
      render to its own target we can capture directly, rather than being classified out of a
      batch? (That would be materially cleaner than the gameswf classifier.)
- [ ] **DR-I8 Resolution lever.** `setres` exec vs
      `My Games\BioShock Infinite\XGame\Config\` `ResX`/`ResY`. On BS1 `SETRES` faulted through the
      viewport Exec seam; do not assume either way. **Acceptance is the backbuffer at first Present
      after a relaunch**, never the config read-back.
- [ ] **Done when:** every DR is recorded pass or fail, with its derivation method.

## I3 - Headset bring-up: mono big screen (~1 session)

Goal: the game on a giant head-tracked screen. Core does essentially all of this.

- [ ] OpenXR session on the game's **own** `ID3D11Device` (zero-copy submits)
- [ ] Quad "cinema screen" layer; pace thread; teardown and alt-tab survival
- [ ] **Cross-check both VDXR and SteamVR.** BS1 never did, and shipped with a Steam Link gap that
      only surfaced later.
- [ ] **Done when:** the Quest 3 shows the game on a head-tracked screen, under both runtimes.

## I4 - 6DoF head camera, and the flat harness (~1.5 sessions)

Goal: a real VR camera, and the ability to test it **without a headset**.

- [ ] Camera driven from the predicted HMD pose, including roll; world scale; recenter
- [ ] **Build `simhead` in this milestone.** On BS1 it arrived at session 12 and the docs name it
      as the thing that should have come first: it drives the entire camera path exactly as a
      headset does, flat, and it is what reproduced the head-coupling bug with no headset at all.
      Include the position triple (BS2's version) so 6DoF is provable flat, not just rotation.
- [ ] **Build record/replay (`vrrec`) in this milestone too.** On BS1 it arrived at session 20.
- [ ] Camera write discipline: BS1 wrote pitch **absolutely** and yaw **relatively**, which froze
      the engine's own view pitch at -88.9 degrees for a whole session and silently broke melee,
      because nothing else wrote it and nothing noticed. Prefer a **servo through the game's own
      input path** over writing engine memory; if writing directly, make sure something reads back
      and something notices.
- [ ] 1 Hz heartbeat logging the FINAL camera (post drive and offsets) so flat checks measure
      numbers directly
- [ ] **Done when:** lean physically around a corner in Columbia with no drift and correct roll,
      user-verified in the headset; and `simhead` reproduces a head-driven camera flat.

## I5 - Lens audit, FOV, resolution, and the config menu (~2-3 sessions)

Goal: **user priority 1.** A correct projection, a resolution picker, and a real config/preset UI.

This is the highest-leverage milestone in the project. On BS1 the projection-claim question caused
the M3 "swim", blocked M4, produced the cinematic fisheye, produced the release-blocking yaw warp
and the "hands are huge" report - and it is *still* the open BS2 blocker at session 33. Front-load
the instrument.

- [ ] **Build the lens decoder first.** Stride-sampled, majority-voted, structurally validated,
      multi-lens. **Assume the frame carries more than one lens until proven otherwise.** Publish
      the runner-up as a named second lens. Refuse to publish a round without a clear majority
      rather than publishing a confident wrong value. BS1's watch took the first decodable draw and
      latched onto the viewmodel frustum, and that single choice was the release blocker.
- [ ] **Derive the lens law from two different backbuffer aspects, never one.** The world and
      foreground conventions coincide exactly at 16:9. That coincidence cost BS1 two sessions and
      BS2 one.
- [ ] FOV lever. The native slider spans only ~70 to ~80.5 degrees (`MaxUserFOVOffsetPercent=15`),
      so a lever is still needed - but the property chain is *named*, so try `set`-by-name before
      any memory scan.
- [ ] Resolution picker: named square-first modes plus custom, live-vs-config comparison, and the
      aspect warning (a headset eye is near-square; a 16:9 render throws away most of its width).
- [ ] **Call `xrEnumerateViewConfigurationViews`.** Neither BS1 nor BS2 ever does, and the docs
      name its absence as the missing input for both a correct FOV policy and a derived render
      target (`recommendedImageRect`).
- [ ] **Extract `bvr::config` into core** with per-key registration and real types. Today the
      preset system is a 60-branch `strcmp` chain in the BS1 adapter that round-trips ints and
      bools through float and persists *core* state from *adapter* code - which is why BS2 writes
      no ini at all. A third consumer is the right trigger.
- [ ] Config menu with **named preset save and load**, so nothing has to be clicked twice.
- [ ] Move the generic half of the resolution ini lane (section-scoped edit, backup, temp file,
      `ReplaceFileW`, read-back) into core alongside it.
- [ ] **Done when:** a full preset round-trips through the menu across a restart; the resolution
      picker's value is confirmed by the backbuffer at first Present; and the claimed projection
      matches the rendered frustum **at a non-16:9 aspect**, measured, not argued.

## I6 - Stereo (~3-5 sessions - the risk milestone)

Goal: true geometric stereo with 6DoF.

Ladder, in this order, because it worked on BS1 and each rung is independently shippable:
MonoScreen, MonoTracked, AlternateEye, then SequentialReentry.

- [ ] Entry gate: I5's lens decoder is trustworthy. A wrong FOV claim makes **every** subjective
      stereo judgment (scale, depth, comfort) worthless, and BS1's M4 was blocked on exactly this.
- [ ] AlternateEye rung (core already supports it) as the de-risking step
- [ ] SequentialReentry: find the scene-build root and call it twice per game tick with per-eye
      cameras. Whether a single-threading rung is needed at all is DR-I5's answer.
- [ ] **Gate pass 2 deny-by-default** on a known gameplay caller return RVA, so doubling can never
      run inside a load path.
- [ ] Pair pacing: one `xrWaitFrame`, one locate, one prediction per game tick. Submitting the two
      eyes of a pair with poses located a compositor period apart produces motion-dependent shear.
- [ ] Camera pass 2 is a **replay of pass 1's cached base**, not a re-sample - a Present lands
      between passes, and re-sampling skews the pair into vertical disparity.
- [ ] **Done when:** true geometric parallax (verified in the headset), 72 fps at default render
      scale, a 30-minute session with no visual state corruption, and stability across a level
      transition and a quit-to-menu.

## I7 - Motion controllers and full control mapping (~2 sessions)

Goal: **user priority 3 and 4.** The game fully playable from the headset, every action bound.

- [ ] OpenXR action set, Quest 3 Touch bindings, synthetic-XInput lane
- [ ] **Extract the XR-to-pad mapping table out of core** into a per-game table the adapter
      supplies. It is currently hardcoded BioShock semantics (B->Y jump, Y->B med hypo, grips to
      bumpers) living in `core/vr/openxr_input.cpp`.
- [ ] Infinite-specific mapping work that BioShock has no analogue for: two-weapon carry (not the
      full wheel), Vigors on the left hand, sprint, hack/lockpick, Elizabeth's item toss, and the
      Sky-Hook melee.
- [ ] **The Skyline (TBar) control family.** The shipped ini documents an `XInputHandler` chain
      mechanism where commands joined by `+` short-circuit on success, with a large TBar set
      (transfers, boost, dodge, melee transfer, zoom, and the highlight-effect pairs). Any VR remap
      must preserve that chain semantics.
- [ ] Drive the named UE3 axes (`aLeftStickX` etc.) directly - a large advantage over BS1, where
      `exec NextWeapon` faulted and weapon switching could not be driven flat at all.
- [ ] Expect the Steam overlay to eat `XInputGetState` at the proxy thunk; the IAT-hijack lane is
      already scoped (slot RVA `0xCD4814`).
- [ ] **Done when:** the game is fully playable from the headset with every action bound, audited
      against the shipped `DefaultInput.ini` command list so nothing is silently missing.

## I8 - Decoupled aim (~2 sessions)

Goal: the controller aims, the head looks.

- [ ] Find UE3's fire path; substitute at the origin-and-direction seam so per-weapon spread still
      applies downstream
- [ ] **The FRotator trap**: rotation units are int32s whose float reinterpretation is a denormal,
      so a live fire direction prints as `(0.000 0.000 0.000)`. This cost BS1 a long detour.
      Classify out-params by value, not by position.
- [ ] Read-only probe hooks (install both, refuse to substitute) so a diagnostic **cannot change
      what it measures**. BS1: "any future 'is this seam involved at all' question should start
      there."
- [ ] Respect `ret imm / 4` for every probe (see the RTC warning in ENGINE_NOTES)
- [ ] Per-weapon aim profiles keyed by class name, covering the DLC weapons
- [ ] **Done when:** look one way and shoot another - impacts land where the controller points,
      user-verified.

## I9 - Viewmodel and hands decoupled from the headset, and scale (~3 sessions)

Goal: **user priority 2.** Hands, weapons and Vigors stuck in space, not glued to the head, at a
believable size.

This was the single largest sink on BS1 (about seven sessions plus a relapse). **Apply the policy
gate before writing a line of compensation code.**

- [ ] Step 1: check what UE3 does natively here.
- [ ] Step 2: **test whether the BS1/BS2 defect even exists.** BS1's viewmodel renders through a
      separate foreground lens; BS2 has a structurally different second pass fixed at 60 degrees.
      Infinite may have neither, one, or a third thing. The identification method to prefer is a
      **falsifiable quantitative prediction** (sweep the FOV option and predict the magnitude of
      the error) over another frame capture - that is what settled BS2 in one round.
- [ ] Step 3: only then port compensation machinery, and only the parts proven necessary.
- [ ] UE3 differences that matter: the skeleton is `USkeletalMeshComponent::SpaceBases`, not a
      Havok `hkQsTransform` array; attachment is components and sockets, not the UE2 Base/Owner
      pair. BS1's bone drive transfers in **shape only**.
- [ ] The principle that does transfer: **engine-side writes are re-read by the engine, so attached
      objects and effects follow for free; render-side matrix patches leave separate objects
      behind** - the Vigor hand FX above all.
- [ ] World scale calibration, and the world/viewmodel scale split if it proves necessary
- [ ] **Test from the START of the game, not only from a loaded-out save** (user directive,
      2026-07-31). A reflection-conjured arsenal is convenient but unrepresentative: the opening
      hours have no weapon at all, then the Sky-Hook alone, then one gun. Calibration that only
      holds with a full loadout is not calibration. The user will produce the combat/weapons save
      at this milestone rather than earlier, precisely so both ends get covered.
- [ ] **Done when:** the weapon is one with the right controller and the Vigor hand one with the
      left, inspectable from any angle, believable size, effects attached, and **they do not move
      with the head** - user-verified in the headset.

## I10 - HUD, menus and effects (~2-3 sessions)

Goal: **user priority 5, part 1.** Readable HUD, usable menus, correct full-screen effects.

- [ ] Scaleform GFx classifier (not the gameswf one - `hud_capture` is a worked example here, not
      a library)
- [ ] Health / Salts / Shield / ammo / Vigor selection on a readable panel in both eyes
- [ ] Pause menu, upgrade menus (Veni Vidi Vigor machines, gear), and the hacking/lockpick UI
- [ ] Full-screen effects. The BS1 rule that generalizes: effects authored in **stage space cannot
      be made full-screen by routing them** to a different target - that needs different geometry.
- [ ] Any backbuffer-content detector must sample **before** our own writers. BS1's letterbox
      detector worked every time flat and never in the headset for exactly this reason.
- [ ] **Done when:** a non-developer can read their health and ammo and navigate the menus from
      inside the headset.

## I11 - Cinematics (~2 sessions)

Goal: **user priority 5, part 2 and 6.** Both cinematic classes work in VR.

- [ ] **Bink FMV** through `binkw32.dll` (100+ `.bik` files: attract movie, credits, voxophone and
      PSA reels, per-Vigor tutorials)
- [ ] **Engine-rendered Matinee** scenes (`MatineeCamera` is in the FName pool)
- [ ] These are two different problems; BS1 only had one. Expect different handling for each.
- [ ] Correct projection claim throughout - BS1's cinematics rendered their own FOV while the claim
      said something else, and read as a fisheye
- [ ] Selectable rig behaviour during cutscenes (authored / authored + head look / off), as BS1
      ships it
- [ ] Subtitles readable in stereo
- [ ] **Done when:** both classes play correctly in stereo, user-verified, with no fisheye and no
      black-bar artifacts.

## I12 - Infinite-specific set pieces and DLC tuning (~2-3 sessions)

Goal: the things BioShock never had, plus the full DLC sweep.

- [ ] **The Skyline.** It is a rollercoaster and a comfort problem with no BioShock analogue.
      Expect to need comfort options (vignette, and possibly a rig-stabilisation mode) that the
      other two games did not.
- [ ] Songbird sequences, Handyman fights, Elizabeth's tears
- [ ] Full tuning sweep across **Burial at Sea 1 and 2 and Clash in the Clouds**: aim profiles for
      the DLC weapons and Old Man Winter, scale, HUD placement, viewmodel calibration
- [ ] **Done when:** a Skyline ride is comfortable, and aim, scale, HUD and viewmodel calibration
      hold in all three DLC.

## I13 - Release (~1-2 sessions)

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
