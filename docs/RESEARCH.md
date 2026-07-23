# Research

Ground-truth findings from session 1 (2026-07-23). Every claim carries its source. New research
appends here; corrections edit in place with a dated note.

## The target

Verified on disk (this machine):

- Install: `K:\SteamLibrary\steamapps\common\BioShock Remastered` (~20.7 GiB, appid 409710).
- Sole executable: `Build\Final\BioshockHD.exe` — **32-bit x86** (PE machine 0x014C), linker
  timestamp 2022-04-13, clean 5-section PE, plain import table, **no DRM/anti-tamper** (plain
  steam_api.dll only).
- Statically imports: `d3d9.dll` (Direct3DCreate9), `d3d11.dll` (D3D11CreateDevice),
  `D3DCOMPILER_43.dll`, `DINPUT8.dll`, `DSOUND.dll`, **`XINPUT1_3.dll`** (ordinals 2, 3),
  `WINMM.dll`, `dbghelp.dll` (minidump crash handler).
- Two renderer classes compiled in: `D3DDrv.D3DRenderDevice` (D3D9) and
  `D3DDrv11.D3DRenderDevice11` (D3D11). Shipped `ShaderCache.pcs11` ⇒ D3D11 is the active
  default (verify at runtime; `GETCURRENTRENDERDEVICE` console command exists).
- Shipped DLLs (all 32-bit): bink2w32 (video), fmodex (FMOD Ex audio), steam_api, amd_ags_x86.
  No d3d proxy DLLs present — the proxy slot is free.
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
  console-command scripts (cheat/test commands demonstrably supported).
- Content: 12 compressed UnrealScript `.U` packages (`Build\Final\BakedScripts\pc`, Unreal
  signature 0x9E2A83C1, package version 142, licensee 56; ShockGame.u = 92 MB), 161 `.bsm` maps,
  `.blk` bulk textures, FSB5 sound banks, `ConfigINI.IBF` bundle (contains Weapons.ini,
  Plasmids.ini, Bindings.ini, Gui.ini…).
- The game has never been launched on this machine — no user ini/save folder exists yet.
- BioShock 2 / Infinite are not installed in any local Steam library.

## Engine family (for future ports)

- **BioShock 2 / 2 Remastered**: same UE2.5/Vengeance lineage, same remaster contractor (Blind
  Squirrel), 32-bit exe + DX11, console on Tab. Adapter port expected cheap.
  https://www.unrealengine.com/blog/bioshock-2 · https://www.pcgamingwiki.com/wiki/BioShock_2_Remastered
- **BioShock Infinite**: **Unreal Engine 3 build 6829** (32-bit, DX11) with large custom
  replacements (deferred renderer, AI, animation). Concepts carry over; offsets/hooks do not.
  https://www.shacknews.com/article/66321/irrational-details-bioshock-infinite-engine ·
  https://www.pcgamingwiki.com/wiki/BioShock_Infinite

## Prior art

| Project | License | What we take |
|---|---|---|
| [itsloopyo/bioshock-remastered-headtracking](https://github.com/itsloopyo/bioshock-remastered-headtracking) (Rust, Nexus mod 144, active 2026) | MIT | **Proven on this exe**: xinput1_3 proxy injection; MinHook; hook of `APlayerController::eventPlayerCalcView(this, view_actor, FVector* loc, FRotator* rot)`; patch-resistant FName-chain scan; FRotator = 3×i32, 65536/turn; live FOV at PlayerController+0xE0; look/aim decoupling. Port techniques to C++ with attribution; keep the Rust build as a scan cross-check. Companion: [cameraunlock-core](https://github.com/itsloopyo/cameraunlock-core) (MIT). |
| [praydog/REFramework](https://github.com/praydog/REFramework) | MIT | Working reference for the whole VR half: OpenXR+OpenVR backends with fallback, D3D11 stereo submission on the game's device, overlay UI, motion controllers, IK arms (RE2/RE3). Code may be adapted with attribution. |
| [praydog/UEVR](https://github.com/praydog/UEVR) + [write-up](https://praydog.com/reverse-engineering/2023/07/03/uevr.html) | **All rights reserved** | Concepts ONLY, zero code. Its core trick (hijacking UE4's IStereoRendering/FFakeStereoRendering) does not exist in UE2.5. Generalizable ideas: string-anchored function finding, attach-to-controller UX, dual-runtime layer. |
| [Nibre/MotherVR](https://github.com/Nibre/MotherVR) (Alien: Isolation) + GRAND-MotherVR (2025) | binaries only | Existence proof: closed-source engine → dxgi-proxy 6DOF VR, later full hands + motion aim ([Road to VR](https://www.roadtovr.com/mothervr-mod-alien-isolation-oculus-rift-nibre-zack-fannon/), [UploadVR](https://www.uploadvr.com/alien-isolations-new-grand-mothervr-mod-adds-hands-brings-qol-improvements/)). They had leftover DK2 code; BioShock has none. |
| Luke Ross R.E.A.L. ([gta5-real-mod](https://github.com/LukeRoss00/gta5-real-mod)) | mixed | Alternate-eye rendering: near-mono GPU cost, ghosting, no motion controls. Known-quantity fallback only. |
| vorpX | commercial | **Supports BioShock Remastered in G3D (full geometry stereo)** — proof the DX11 renderer tolerates dual-view rendering ([PCGamingWiki VR row](https://www.pcgamingwiki.com/wiki/BioShock_Remastered), [vorpX forums](https://www.vorpx.com/forums/topic/bioshock-12-remastered-low-fps/)). Also Z3D + DirectVR (memory-written head tracking). |
| Helix / 3Dmigoto / geo-11 | GPL / closed | 3D Vision fix exists for the **DX9 original only** ([Helix](https://helixmod.blogspot.com/2013/02/bioshock.html)); none for the remaster — shaderhackers say remaster shaders need per-shader stereo fixes and nobody invested (~20 h+). geo-11 is closed-source binaries ([releases](https://github.com/ThreeDeeJay/geo-11/releases)). This is why we do NOT bet on shader-patch stereo. |
| [Vireio Perception](https://github.com/cybereality/Perception) | LGPL-3 | Historic open-source stereo injector (did original BioShock, DX9). Reference only. |
| HL2VR / Doom3BFG-VR / Team Beef | source ports | Not applicable — no BioShock source. Explains why "native-feel" mods usually recompile the game; we can't. |

No native VR mod (stereo + motion controls) exists for any BioShock game. UEVR does not apply.

## Engine access routes

- **UnrealScript decompilation**: [UELib](https://github.com/EliotVU/Unreal-Library) /
  [UE Explorer](https://github.com/UE-Explorer/UE-Explorer) explicitly support BioShock (build
  "2226:Vengeance", package versions 130–141), BioShock 2 (143), Infinite (6829). Best route to
  reconstruct class/property layouts. **Decompiled output is 2K copyright — never committed;
  findings are summarized in ENGINE_NOTES.md.**
- **Adjacent source**: SWAT 4 (same Vengeance engine) full UnrealScript + editor source via
  [SWAT: Elite Force](https://github.com/eezstreet/SWATEliteForce) — closest public reference
  to engine internals.
- **Asset tools**: umodel, [UPK Explorer](https://www.nexusmods.com/site/mods/587) + TFC
  Installer ([method article](https://www.nexusmods.com/bioshock/articles/1)) — property/texture
  patch injection (full repack impossible). Not needed for the VR mod's core but useful for
  experiments.
- **Console**: `-allowconsole` launch option, Tab in game; `SetFOV`, `FreeCamera`, `ToggleHUD`,
  `God`, `SloMo`… ([command guide](https://steamcommunity.com/sharedfiles/filedetails/?id=842210214)).
  Mixed community reports that some newer builds disable it — verify on first launch; fallback
  is User.ini binds at `%AppData%\Roaming\BioshockHD\Bioshock`.
- Community: BioShock Modding Discord; Nexus Mods bioshock section.

## VR delivery (Quest 3)

- **OpenXR-first**: `XR_KHR_D3D11_enable`; `xrGetD3D11GraphicsRequirementsKHR` before session
  creation; session on the game's own ID3D11Device (zero-copy submits); `xrWaitFrame` →
  `xrLocateViews` (predicted display time) → `xrEndFrame` with projection + quad layers.
  Spec: https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
- **Virtual Desktop** ships its own OpenXR runtime, **VDXR** (from mbucchia's
  VirtualDesktopXR) — an OpenXR client uses it natively, bypassing SteamVR (~10% faster).
  https://www.uploadvr.com/virtual-desktops-vdxr-runtime/
- **Steam Link** requires SteamVR; SteamVR's OpenXR runtime serves the same client.
- **Open question (DR-1)**: whether VDXR ships a working **32-bit** runtime DLL. OpenXR
  supports Win32 clients in spec; per-runtime support must be tested. Fallbacks: SteamVR-only →
  64-bit companion compositor (shared textures + pose shm).
- vrto3d / geo-11+Katanga are "3D theater screen" pipelines (no world-scale tracking) — not our
  path; possible debug milestone only.

## Legal / distribution posture

- Ship injector-only: our own DLLs, no game assets, no decompiled code, free and open source.
- Take-Two has DMCA'd **paid** VR mods (Luke Ross GTA/RDR2/Mafia, July 2022 —
  [PC Gamer](https://www.pcgamer.com/take-two-has-been-issuing-takedowns-for-gta-mods/));
  free open-source BioShock mods and tools have remained up. Stay free, unaffiliated, asset-clean.
