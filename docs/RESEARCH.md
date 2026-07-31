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
