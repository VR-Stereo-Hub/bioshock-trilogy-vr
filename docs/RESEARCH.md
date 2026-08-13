# Research

Ground-truth findings from session 1 (2026-07-23). Every claim carries its source. New research
appends here; corrections edit in place with a dated note.

## The target

Verified on disk (this machine):

- Install: `K:\SteamLibrary\steamapps\common\BioShock Remastered` (~20.7 GiB, appid 409710).
- Sole executable: `Build\Final\BioshockHD.exe` - **32-bit x86** (PE machine 0x014C), linker
  timestamp 2022-04-13, clean 5-section PE, plain import table, **no DRM/anti-tamper** (plain
  steam_api.dll only).
- Statically imports: `d3d9.dll` (Direct3DCreate9), `d3d11.dll` (D3D11CreateDevice),
  `D3DCOMPILER_43.dll`, `DINPUT8.dll`, `DSOUND.dll`, **`XINPUT1_3.dll`** (ordinals 2, 3),
  `WINMM.dll`, `dbghelp.dll` (minidump crash handler).
- Two renderer classes compiled in: `D3DDrv.D3DRenderDevice` (D3D9) and
  `D3DDrv11.D3DRenderDevice11` (D3D11). Shipped `ShaderCache.pcs11` ⇒ D3D11 is the active
  default (verify at runtime; `GETCURRENTRENDERDEVICE` console command exists).
- Shipped DLLs (all 32-bit): bink2w32 (video), fmodex (FMOD Ex audio), steam_api, amd_ags_x86.
  No d3d proxy DLLs present - the proxy slot is free.
- Engine: **Vengeance Engine 2** = heavily modified **Unreal Engine 2.5** (Tribes: Vengeance /
  SWAT 4 lineage) with UE3 features backported; the remaster added a D3D11 multithreaded
  renderer. Evidence: Jenkins build paths `source\unreal\{core,engine,d3ddrv,d3ddrv11}` in the
  exe; RTTI names `AVengeanceGameInfo`, `AShockPlayer`, `AShockPlayerController`, `AShockHUD`,
  `AHands`, `AWeapon`. Sources: https://en.wikipedia.org/wiki/Vengeance_Engine ·
  https://bioshock.fandom.com/wiki/BioShock_Technical_Information ·
  https://www.pcgamingwiki.com/wiki/BioShock_Remastered
- **Havok Physics 2012.2.0 r1** statically linked. **UI (menus + HUD) is Flash**: .swf files in
  `ContentBaked\pc\FlashMovies` (HUDRadial.swf, pause.swf, hacking.swf…) rendered by an embedded
  **gameswf** runtime (not Scaleform).
- Config: `[Engine.Console]` ConsoleKey=9 (Tab); `[Engine.RenderConfig]`
  **HorizontalFOVLock=True**, `UseMultithreadedRendering=False` (single render thread by
  default); no player-FOV ini setting exists. Input runs through Unreal axis aliases
  (aBaseX/aLookUp) in DefUser.ini. `.debug` files in `ContentBaked\pc\System` are plaintext
  console-command scripts - a reference for the engine's command vocabulary.
- Content: 12 compressed UnrealScript `.U` packages (`Build\Final\BakedScripts\pc`, Unreal
  signature 0x9E2A83C1, package version 142, licensee 56; ShockGame.u = 92 MB), 161 `.bsm` maps,
  `.blk` bulk textures, FSB5 sound banks, `ConfigINI.IBF` bundle (contains Weapons.ini,
  Plasmids.ini, Bindings.ini, Gui.ini…).
- The game has never been launched on this machine - no user ini/save folder exists yet.
- ~~BioShock 2 / Infinite are not installed in any local Steam library.~~ **Superseded**: BioShock 2
  Remastered was installed for M10 (session 24), and **BioShock Infinite is installed** at
  `D:\SteamLibrary\steamapps\common\BioShock Infinite` with all DLC (verified session 34).

## Engine family (for future ports)

- **BioShock 2 / 2 Remastered**: same UE2.5/Vengeance lineage, same remaster contractor (Blind
  Squirrel), 32-bit exe + DX11, console on Tab. Adapter port expected cheap.
  https://www.unrealengine.com/blog/bioshock-2 · https://www.pcgamingwiki.com/wiki/BioShock_2_Remastered
- **BioShock Infinite**: **Unreal Engine 3 build 6829** (32-bit, DX11) with large custom
  replacements (deferred renderer, AI, animation). Concepts carry over; offsets/hooks do not.
  https://www.shacknews.com/article/66321/irrational-details-bioshock-infinite-engine ·
  https://www.pcgamingwiki.com/wiki/BioShock_Infinite
  **PROMOTED to an active project 2026-07-31 (session 34)** - branch `bioshock-infinite`, see
  [bioshockinfinite/ROADMAP.md](bioshockinfinite/ROADMAP.md) and
  [bioshockinfinite/ENGINE_NOTES.md](bioshockinfinite/ENGINE_NOTES.md). Session-34 recon,
  read-only, all verified against the shipped files:
  - x86 32-bit, **fixed ImageBase 0x00400000 with ASLR OFF** (both remasters are rebased), LAA yes.
  - Imports `XINPUT1_3.dll` **by ordinal 2 and 3**, same as both remasters, so `src/proxy/` works
    verbatim; game IAT slot for ord 2 at RVA `0xCD4814`. `d3d11`/`dxgi` are loaded dynamically.
  - Full UE3 reflection intact (FName pool carries `PlayerController`, `GetPlayerViewPoint`,
    `CheatManager`, `Scaleform`, `MatineeCamera`, ...), so **BS2's ProcessEvent-by-name seam is the
    natural camera hook**. No `CalcView` name - that is the Vengeance spelling.
  - **A working cheat path exists**, unlike both remasters: the shipped `DefaultInput.ini` has a
    live "Debug binds" block (`god`, `ghost`, `preventdeath`, `walk`, `viewmode`, `shot`) and a
    bind proving `set <class> <prop> <value>` works. Not yet confirmed live.
  - `OneFrameThreadLag=True` in `BaseEngine.ini` is a **config-level analogue of BS1's
    `reentry 1t`**, potentially without a flush-point hook.
  - Native FOV slider exists but caps at +15% (~70 to ~80.5 deg), so a lever is still needed.
  - `[Stereoscopic3D]` exists in the ini, but **no `bStereo`/`EyeSeparation`/`StereoDevice` names
    are in the exe**, pointing at driver-side 3D Vision rather than an engine per-eye path. Plan
    for SequentialReentry; timebox the check.
  - UI is **Scaleform GFx**, not gameswf, so `hud_capture` is a worked example, not a library.
    Cinematics split into Bink FMV (`binkw32.dll`, 100+ `.bik`) and engine Matinee.

## Prior art

| Project | License | What we take |
|---|---|---|
| [itsloopyo/bioshock-remastered-headtracking](https://github.com/itsloopyo/bioshock-remastered-headtracking) (Rust, Nexus mod 144, active 2026) | MIT | **Proven on this exe**: xinput1_3 proxy injection; MinHook; hook of `APlayerController::eventPlayerCalcView(this, view_actor, FVector* loc, FRotator* rot)`; patch-resistant FName-chain scan; FRotator = 3×i32, 65536/turn; live FOV at PlayerController+0xE0; look/aim decoupling. Port techniques to C++ with attribution; keep the Rust build as a scan cross-check. Companion: [cameraunlock-core](https://github.com/itsloopyo/cameraunlock-core) (MIT). |
| [praydog/REFramework](https://github.com/praydog/REFramework) | MIT | Working reference for the whole VR half: OpenXR+OpenVR backends with fallback, D3D11 stereo submission on the game's device, overlay UI, motion controllers, IK arms (RE2/RE3). Code may be adapted with attribution. |
| [praydog/UEVR](https://github.com/praydog/UEVR) + [write-up](https://praydog.com/reverse-engineering/2023/07/03/uevr.html) | **All rights reserved** | Concepts ONLY, zero code. Its core trick (hijacking UE4's IStereoRendering/FFakeStereoRendering) does not exist in UE2.5. Generalizable ideas: string-anchored function finding, attach-to-controller UX, dual-runtime layer. |
| [Nibre/MotherVR](https://github.com/Nibre/MotherVR) (Alien: Isolation) + GRAND-MotherVR (2025) | binaries only | Existence proof: closed-source engine → dxgi-proxy 6DOF VR, later full hands + motion aim ([Road to VR](https://www.roadtovr.com/mothervr-mod-alien-isolation-oculus-rift-nibre-zack-fannon/), [UploadVR](https://www.uploadvr.com/alien-isolations-new-grand-mothervr-mod-adds-hands-brings-qol-improvements/)). They had leftover DK2 code; BioShock has none. |
| Luke Ross R.E.A.L. ([gta5-real-mod](https://github.com/LukeRoss00/gta5-real-mod)) | mixed | Alternate-eye rendering: near-mono GPU cost, ghosting, no motion controls. Known-quantity fallback only. |
| vorpX | commercial | **Supports BioShock Remastered in G3D (full geometry stereo)** - proof the DX11 renderer tolerates dual-view rendering ([PCGamingWiki VR row](https://www.pcgamingwiki.com/wiki/BioShock_Remastered), [vorpX forums](https://www.vorpx.com/forums/topic/bioshock-12-remastered-low-fps/)). Also Z3D + DirectVR (memory-written head tracking). |
| Helix / 3Dmigoto / geo-11 | GPL / closed | 3D Vision fix exists for the **DX9 original only** ([Helix](https://helixmod.blogspot.com/2013/02/bioshock.html)); none for the remaster - shaderhackers say remaster shaders need per-shader stereo fixes and nobody invested (~20 h+). geo-11 is closed-source binaries ([releases](https://github.com/ThreeDeeJay/geo-11/releases)). This is why we do NOT bet on shader-patch stereo. |
| [Vireio Perception](https://github.com/cybereality/Perception) | LGPL-3 | Historic open-source stereo injector (did original BioShock, DX9). Reference only. |
| HL2VR / Doom3BFG-VR / Team Beef | source ports | Not applicable - no BioShock source. Explains why "native-feel" mods usually recompile the game; we can't. |

No native VR mod (stereo + motion controls) exists for any BioShock game. UEVR does not apply.

## Engine access routes

- **UnrealScript decompilation**: [UELib](https://github.com/EliotVU/Unreal-Library) /
  [UE Explorer](https://github.com/UE-Explorer/UE-Explorer) explicitly support BioShock (build
  "2226:Vengeance", package versions 130–141), BioShock 2 (143), Infinite (6829). Best route to
  reconstruct class/property layouts. **Decompiled output is 2K copyright - never committed;
  findings are summarized in ENGINE_NOTES.md.**
- **Adjacent source**: SWAT 4 (same Vengeance engine) full UnrealScript + editor source via
  [SWAT: Elite Force](https://github.com/eezstreet/SWATEliteForce) - closest public reference
  to engine internals.
- **Asset tools**: umodel, [UPK Explorer](https://www.nexusmods.com/site/mods/587) + TFC
  Installer ([method article](https://www.nexusmods.com/bioshock/articles/1)) - property/texture
  patch injection (full repack impossible). Not needed for the VR mod's core but useful for
  experiments.
- **Console**: legacy `-allowconsole` + Tab, plus engine commands like `SetFOV`, `ToggleHUD`.
  RESOLVED session 9: the in-game console is compiled out of this Steam build and key-bound
  commands are inert (verified live), so the mod issues engine commands by calling the
  engine's own Exec dispatchers directly (`exec` seam - ENGINE_NOTES "Gamepad architecture").
- Community: BioShock Modding Discord; Nexus Mods bioshock section.

## VR delivery (Quest 3)

- **OpenXR-first**: `XR_KHR_D3D11_enable`; `xrGetD3D11GraphicsRequirementsKHR` before session
  creation; session on the game's own ID3D11Device (zero-copy submits); `xrWaitFrame` →
  `xrLocateViews` (predicted display time) → `xrEndFrame` with projection + quad layers.
  Spec: https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
- **Virtual Desktop** ships its own OpenXR runtime, **VDXR** (from mbucchia's
  VirtualDesktopXR) - an OpenXR client uses it natively, bypassing SteamVR (~10% faster).
  https://www.uploadvr.com/virtual-desktops-vdxr-runtime/
- **Steam Link** requires SteamVR; SteamVR's OpenXR runtime serves the same client.
- **Open question (DR-1)**: whether VDXR ships a working **32-bit** runtime DLL. OpenXR
  supports Win32 clients in spec; per-runtime support must be tested. Fallbacks: SteamVR-only →
  64-bit companion compositor (shared textures + pose shm).
- vrto3d / geo-11+Katanga are "3D theater screen" pipelines (no world-scale tracking) - not our
  path; possible debug milestone only.

## Legal / distribution posture

- Ship injector-only: our own DLLs, no game assets, no decompiled code, free and open source.
- Take-Two has DMCA'd **paid** VR mods (Luke Ross GTA/RDR2/Mafia, July 2022 -
  [PC Gamer](https://www.pcgamer.com/take-two-has-been-issuing-takedowns-for-gta-mods/));
  free open-source BioShock mods and tools have remained up. Stay free, unaffiliated, asset-clean.

## BioVRDev/Bioshock-Remastered-VR analysis (2026-07-28)

Source: https://github.com/BioVRDev/Bioshock-Remastered-VR (released
~2026-07-13, analyzed at commit of 2026-07-28). **NO LICENSE file = all
rights reserved: concepts and measurements only, never code** - same
boundary as UEVR. Their README credits this repo for the reticle disable
(console SET path), the arm-hide bone indices, and the HUD render-target
capture.

**Architecture**: dxgi.dll proxy; hooks Present + eventPlayerCalcView (same
seams as ours). Stereo = ALTERNATE EYE per game frame at full game rate
(consecutive frames pair L/R, ~4.2 ms apart), with a pair-locked camera
(eye 1 re-renders from eye 0's snapshot), head pose latched once per pair,
and the projection layer stamped with the pose the image was rendered from.
Model drive = ABSOLUTE writes to the AHands ACTOR Rotation/Location every
CalcView (the approach we retired for the bone drive) + a render-thread
roll re-write (the game erases roll between tick and render). No fire
hook: they write the PlayerController rotation (clamped +-20 deg vs head,
EMA 0.35) and let the game shoot; the VR dot is an OpenXR quad from the
same offset table, so dot==shot by construction and their calibration flow
("fire at a wall, nudge the dot onto the bullet hole") is exact.

**Scale**: numerically identical to ours - 1 UU = 1 cm hard-coded (=
worldScale 100), half-IPD default 3.2 cm as a camera-location offset along
the final right vector, head position at x100, NO rig scaling (all scale
knobs default off). The size percept works because of two disciplines:
(1) FOV-EXACT SUBMISSION - the game render FOV is locked via Bioshock.ini
(+ HorizontalFOVLock) and the layer is tagged with a SYMMETRIC frustum
built from exactly that FOV and the buffer aspect, never the runtime's
asymmetric per-eye fov ("the headset's frustum is canted and asymmetric,
and that lie is what we remove"); their measured mismatch symptom: "yaw
warped, pitch stayed clean". (2) CYCLOPEAN HAND ANCHORING - hands placed
once per pair at the camera center captured BEFORE the per-eye offset;
per-eye placement "cancels their disparity exactly ... zero parallax means
very far away" (reads huge).

**Per-weapon system**: three vec3s per weapon slot (GripOffset position cm,
RotOffset model quat-compose, CursorOffset aim quat-compose), keyed by
resolving the held weapon's UClass (holdable+0x30) against
ShockPawn.AllPossibleWeaponClasses (found by TArray shape-matching);
hot-swapped on switch, numpad-tuned live, self-saving. 5 of 9 slots
hand-tuned at release. Offsets are resolution/FOV-specific (their README:
the cross-resolution scaling law "was ruled out"; they rescale right/up by
tan(fgNow/2)/tan(fgRef/2) at startup only).

**What they did NOT solve** (independent confirmation of our walls): the
rendered-gun-vs-dot drift (same foreground-projection-vs-compositor split;
their code admits the dot "can drift from the rendered gun"; only static
offset rescaling exists), idle sway (README: "remains"; an experimental
IdlingHandsAnim TArray rewrite at holdable+0x458 ships default-off with a
hang warning), attach-bone scale (their comment mirrors our session-16
1/0 finding), fg FOV (same formula as our vrfgfov, generalized for aspect:
2*atan(tan(fov/2)*(4/3)/aspect)). Useful extra levers they found:
head-bob removal by re-basing the camera on Pawn.Location + EyeHeight
(static field, ~+0x550 area) instead of the animated camera; the fg fov
offset found by float snapshot/diff driven by pistol zoom;
ZoomedForegroundFOVAngle is why they unbound ADS.

**Takeaways adopted into our plan** (STATUS next steps): the FOV audit
(submitted frustum must equal the rendered one exactly), cyclopean/rig
placement verification (subsumed by the world-pass re-homing experiment),
per-weapon profiles + the exact wall-calibration flow, pawn-eye-point
anchoring as the walk-bob decoupling lever.

## Session 43: the Infinite stutter hunt - prior art and levers (2026-08-06)

Context: the s42 VDXR headset run falsified the pacing-beat theory (steady pairs ==
refresh, sd 0.3-1 ms) and named the judder as recurring HITCH SPIKES: 39-113 ms pair
intervals in bursts, worse outdoors at native 2064x2208, load-sensitive, binding to head
turns. Scope directive: fix at native res - streaming/GC/shader/scheduling class only.
Three research sweeps (vanilla-game community fixes, VR-mod prior art, UE3 internals)
ran before any code/ini change; findings with sources below, ranked experiments at the
end. UEVR findings are CONCEPTS ONLY (all-rights-reserved); REFramework is MIT
(adaptable with attribution).

### A. What this UE3 build (6829, "Icarus") actually does

- **Texture pool**: PC pool size is AUTO-CALCULATED at boot (detected VRAM minus frame
  buffers minus `TexturePoolSizeReductionMB=40`); the ini `PoolSize=400` is read ONLY
  with `-ReadTexturePoolFromIni` on the command line, or the pool is removed entirely
  with `-DisableTexturePool` (both semi-official - relayed by 2K support; the ini
  comment in `XGame\Config\DefaultEngine.ini` says the same). Sources:
  https://steamcommunity.com/sharedfiles/filedetails/?id=161397496 ,
  https://www.pcgamingwiki.com/wiki/BioShock_Infinite ,
  https://steamcommunity.com/app/8870/discussions/0/618456760265869551/ (root-caused
  area-load hitches; `-DisableTexturePool` "reduced the stutter about 90 percent").
  **The load-bearing corollary for VR**: the boot auto-calc knows NOTHING about
  allocations made after boot - our two 2064x2208 XR swapchains, the mirror, and UE3's
  own scene targets grown to VR resolution. Pool + our targets can oversubscribe VRAM;
  WDDM then pages at submit time - a hitch class that worsens exactly on view change.
- **Streaming IO**: `UseTextureFileCache=TRUE` - mip loads are TFC seek+read; the
  community fix bundle flips it False (with a raised pool) to kill seek hitches.
- **GC**: Infinite ships `TimeBetweenPurgingPendingKillObjects=30` in `[Engine.Engine]`
  (twice the UDK-default rate: a FULL blocking mark-and-sweep every 30 s of gameplay;
  UE3's mark phase is monolithic on the game thread - order 100 ms on 2013-era object
  counts). Irrational already tuned the disregard pool (`[Core.System]
  MaxObjectsNotConsideredByGC=50500`). Sources: UDK MemoryDebugging docs,
  https://topic.alibabacloud.com/a/reprint-unreal-engine-3-ue3-garbage-collection-mechanism_8_8_31101402.html
  (mark traverse 0.28 s measured on ~90k objects; 0.12 s after disregard tuning),
  https://dev.epicgames.com/documentation/unreal-engine/incremental-garbage-collection-in-unreal-engine
  (reachability was single-frame-blocking until UE5.4).
- **Level streaming**: when a streamed sub-level flips visible, the GAME thread does the
  "make visible" work in one frame (component registration, actor init - Epic's own
  Level Streaming Hitching Guide names this the classic hitch source). Infinite has a
  purpose-built amortizer for exactly this - `[OpportunisticAsyncLoading]`
  (Irrational-specific, undocumented elsewhere: BackgroundAsyncPackageLoadingQuantumMS,
  BackgroundAddToWorldQuantumMS, iteration caps) - **and ships it DISABLED**
  (`bOpportunisticAsyncLoadingEnabled=FALSE`). Community reports of positionally
  deterministic hitches ("always occurs in same place", worse outdoors, sprinting
  across a boundary can crash) match sub-level boundaries. Sources:
  https://dev.epicgames.com/community/learning/tutorials/qpll/unreal-engine-level-streaming-hitching-guide ,
  https://steamcommunity.com/app/8870/discussions/0/828934424230653249/
- **Shader/material first-use hitches are a SEPARATE class**: Irrational's own Technical
  Director recommended `bInitializeShadersOnDemand=True` (every instance, XEngine.ini
  `[SystemSettings]`, reportedly also XCompat.ini) for freeze-frame stutter on first
  appearance of effects/enemies; DXVK-async removing Infinite stutter corroborates a
  driver-side pipeline-creation component. Sources:
  https://steamcommunity.com/app/8870/discussions/0/828937420193881844/ ,
  https://steamcommunity.com/app/8870/discussions/0/3047235828267171330/
- **Frame caps interact with HITCH frequency**, not just avg fps (uncapped high-refresh
  causes multi-second transition hangs; 60 fps locks resolved them):
  https://steamcommunity.com/app/8870/discussions/0/828936718814638699/
- **Streaming pre-warm APIs exist first-class in UE3**: `PrestreamTextures()` and
  `UTexture2D::SetForceMipLevelsToBeResident(seconds)` (UDK TextureStreaming docs) -
  documented for exactly the "about to come on screen" case. No prior art of a VR mod
  widening the streaming frustum for pre-warm (would be our own invention).
- **Stat/diag execs**: `stat streaming`, `stat levels`, `stat memory`, `listtextures`,
  `obj garbage` are C++ exec handlers, present in shipped same-generation UE3 titles
  (Borderlands 2 verified); Infinite's own XGame.ini debug menu binds
  "Streaming Stats:stat streaming". Each is a candidate live diagnostic through our
  bsiexec lane (verify per command by effect - this build killed SCRIPT execs, these
  are C++). Warning from the debunk pile: `bUseBackgroundLevelStreaming=False` "stops
  most hitching" but BREAKS later loads - diagnostic only, never ship.

### B. VR-side prior art (how mods live with engine hitches)

- **Nobody masks 40-120 ms stalls better than the compositor already does.** OpenXR
  spec: after a missed frame the next xrWaitFrame blocks until the stale frame is
  consumed, then the app re-syncs. Meta compositor: a missed frame is re-displayed with
  rotational TimeWarp; "occasional missed frames go largely unnoticed" - only SUSTAINED
  misses judder. ASW engages only after seconds of sustained shortfall (then locks half
  rate); Virtual Desktop SSW likewise (headset-side extrapolation, forces half rate
  when engaged) - neither trips on sporadic 3-9 frame gaps, which ride plain ATW.
  Sources: https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrWaitFrame.html ,
  https://developers.meta.com/horizon/documentation/unity/os-missed-frames/ ,
  https://www.uploadvr.com/virtual-desktop-synchronous-spacewarp/
- **UEVR** (concepts only): no stall-masking machinery at all; delegates to runtime
  reprojection and advises ASW/motion-smoothing OFF for rate-locked games. Its
  "Synchronized Sequential" render mode is the analog of our SequentialReentry.
  https://github.com/praydog/UEVR/blob/master/README.md , https://docs.uevr.io/
- **vorpX** (concepts only): the one true "submit-side absorption" prior art - a
  decoupled render-to-headset thread that keeps presenting with fresh poses while the
  game stalls ("async" mode).
  https://www.vorpx.com/forums/topic/fluid-sync-vs-async-whats-the-differences/
- **Luke Ross REAL** (concepts only): AER - every frame, one eye is a rotational
  reprojection of stale content; evidence that brief stale-rotation content is
  perceptually acceptable.
- **HL2VR**: fixed the CAUSE (shipped dxvk-async for shader-compile stutter), did not
  mask. https://www.pcgamingwiki.com/wiki/Half-Life_2:_VR_Mod
- **VD encoder discrimination**: the VD performance overlay separates Game Time /
  Encoding / Network / Decoding per frame - an app hitch shows in Game Time, an encoder
  hitch spikes Encoding with Game flat. Known Quest 3 encoder spike signatures: AV1 at
  high bitrate (drop to ~120 Mbps or use HEVC 10-bit ~150). VDXR does not expose
  encoder timing through OpenXR frame timing - the overlay is the instrument.
  https://vrdiscord.com/guides/quest-wireless/virtualdesktop.html

### C. The ranked experiment list (lever -> expected effect -> flat measurement)

Metric for every A/B: the s43 spike instrument (spike count/min + worst interval on the
TRACE pairs line, per-phase attribution per spike), over a matched scripted protocol:
80 Hz sim, native 2064x2208, same save, 60 s static + a fixed `head orbit` sweep set,
indoor vs outdoor. Diagnostics are reverted after reading; a lever ships only when the
CAUSE it matches is named by the instrument (user gate, 2026-08-06).

1. **Texture pool sizing** (`-ReadTexturePoolFromIni` + `PoolSize` raised from 400 in
   +200 steps toward ~1024-1536; game-folder DefaultEngine.ini is the propagation
   source for boot-derived XEngine.ini). Expected: fewer/shorter view-change spikes if
   the auto-calc pool is thrashing (evictions + TFC reloads on every turn). 32-bit LAA
   caveat: community-safe values cluster 1500-2048 MAX; our process also holds the XR
   runtime - tune against VA headroom, watch commit. `-DisableTexturePool` is the
   60-second diagnostic form (90 percent improvement reports).
2. **`bInitializeShadersOnDemand=True`** (all instances). Expected: collapses
   FIRST-ENCOUNTER spikes only (new area/effect); zero effect on repeat-visit spikes.
   Cheap, quality-neutral, TD-endorsed - land early, measure by first-vs-revisit sweep.
3. **GC cadence discrimination, then scheduling**: the 30 s
   TimeBetweenPurgingPendingKillObjects timer predicts time-periodic spikes REGARDLESS
   of view; `obj garbage` (if live) reproduces the spike on demand and prices it.
   If confirmed: raise the interval (30 -> 90/120) and/or fire GC at quiet moments from
   the adapter. Expected: removes the view-independent periodic residue.
4. **Streaming pre-warm** (`PrestreamTextures` / `SetForceMipLevelsToBeResident` via
   the reflection lane, around head yaw and known boundaries). Expected: moves mip
   loads ahead of the turn; needs lever 1 first (a full pool would evict the pre-warm).
5. **[OpportunisticAsyncLoading] enable** (bOpportunisticAsyncLoadingEnabled=TRUE,
   AssumedFPSWhenVSynced=80, conservative quanta 2-4 ms). Expected: amortizes
   AddToWorld spikes across frames. Experimental (ships disabled; unknown VR
   interactions) - only after the instrument names AddToWorld (spike outside our
   phases + boundary-deterministic).
6. **Stall-exit pacing hygiene in our lane** (audit, not new machinery): after a game
   stall, never burst backlogged pairs - one clean re-anchor to the next period (the
   pace-sync resync path already does this; verify with the spike instrument that
   post-spike intervals return to period within one pair). Keep VD SSW on Auto/Off for
   headset A/Bs so a sporadic hitch never triggers a half-rate lock.
7. **Submit-side re-submit thread** (vorpX concept; REFramework MIT plumbing if ever
   needed): HIGH risk, mostly duplicates compositor ATW for the projection layer -
   justified only if quad-layer pose-staleness during stalls is the residual complaint
   after 1-6.

Debunked/rejected: `bUseBackgroundLevelStreaming=False` (breaks saves/loads), PoolSize
from system RAM, PoolSize without the launch flag (silent no-op), quality reductions
(out of scope by directive - and community reports agree they do not fix hitches
anyway).
