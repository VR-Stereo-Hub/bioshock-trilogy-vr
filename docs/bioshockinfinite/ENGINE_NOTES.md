# Engine notes - BioShock Infinite (BioShockInfinite.exe)

Single source of truth for everything we know about **BioShockInfinite.exe** internals.

Unlike [../bioshock2/ENGINE_NOTES.md](../bioshock2/ENGINE_NOTES.md), this is **not a sibling of a
sibling**. BioShock 1 and 2 Remastered are the same Vengeance (UE2.5) tree built sixteen minutes
apart. BioShock Infinite is **Unreal Engine 3, build 6829**, with large custom replacements
(deferred renderer, AI, animation) by Irrational, whose game module namespace is `XCore` / `XGame`.
Nothing numeric transfers. What transfers is the *methodology*, which is documented in full in
[../bioshock1/ENGINE_NOTES.md](../bioshock1/ENGINE_NOTES.md) and summarized in the recipes section
below.

Rules:

- Every address/offset used by code lives ONLY in `src/game/bioshockinf/patterns.h/.cpp`, and every
  one is documented here with its derivation method, so it can be re-derived after a game patch.
- **NEVER copy a number from the BS1 or BS2 notes.** Different engine, different link. On this game
  even *shapes* are suspect: BS1/BS2 shapes come from UE2.5, and UE3 restructured `UObject`,
  `FName`, the actor attachment model and the skeleton representation.
- No game-derived content in the repo. No decompiled UnrealScript, no extracted assets, no
  RenderDoc captures. Findings are summarized here; disassembly stays in the gitignored scratchpad.

## The policy gate (user directive, carried from BS2)

**BioShock Infinite is not bound by BS1's or BS2's methods.** Much of the BS1 machinery - the
foreground/viewmodel FOV counter-modeling, weapon scaling compensation, the aim-seam workarounds,
the whole `reentry 1t` single-threading apparatus - exists because of BS1-specific limitations,
not because it is the right design. BS2 already deleted three of those wholesale by checking first.

For every subsystem brought over, in order:

1. Check what UE3 / Infinite does **natively**.
2. Test whether the BS1/BS2 **defect even exists** here.
3. Only then port compensation machinery, and only the parts proven necessary.

A mono screenshot is not a sufficient check for a lens question.

## Rules carried over (the distilled cost of 33 sessions on the remasters)

These are not style preferences. Each one is a bug that shipped.

- **A verified write is not an honoured one.** Acceptance is a measured downstream effect - the
  backbuffer at first Present, the visible behaviour, the ammo counter - never the write's own
  confirmation. `-> HANDLED` from an engine `Exec` proves only that the command was *recognised*.
  BS1 logged `HANDLED` for eight sessions on a `set` that never did anything.
- **Never copy a number between games**, and the rule extends to shapes: a shape-match on the
  wrong game is a mislabel waiting to happen.
- **An instrument that cannot fail its own hypothesis is not evidence.** A negative result from an
  instrument never observed to fire is worth nothing - ship a positive control.
- **A single sample assumes homogeneity.** A frame is not homogeneous. Sample strided, vote, and
  refuse to publish without a clear majority. Publish the runner-up as a named second thing.
- **A measured value must carry the identity of what it measured**, not merely its freshness. A
  source tag says where a number came from, never that it is right.
- **A counter is not evidence until you know which population it counts.**
- **Measure first.** On BS1, one read-only command killed a fix that was about to be built.
- **Prefer a falsifiable prediction over another capture** whenever the arithmetic offers one.
- **Derive layouts and lens laws at more than one aspect.** World and foreground conventions
  coincide exactly at 16:9. That coincidence cost BS1 two sessions and BS2 one.
- **Identify a render pass by making it MOVE**, never by draw counts.
- **Every new render lever ships default OFF**, with a live A/B toggle so a headset report can be
  bisected in-session without a rebuild.
- **One ray.** Anything visible that claims to point where shots go is derived from the identical
  ray, never a sibling copy. Rotation offsets compose as quaternions in the controller's local
  frame; euler adds after conversion are banned (correct at exactly one orientation).
- **Engine-side writes let attachments and effects follow for free; render-side matrix patches do
  not.** This single principle decided BS1's entire viewmodel architecture.
- **Fail soft.** A failed scan or hook logs and lets the game run flat.
- **Stopping and handing back are different operations.** Stopping is always safe. Handing state
  back writes engine memory and needs a live-world interlock. BS1 hung a save-load by releasing
  bones through the previous level's freed skeleton - and SEH did not catch it, because the pages
  were still mapped, just owned by something else.
- **Never take a reference to an engine D3D object from inside a detour.** The game holds its own
  reference for as long as it is bound. Two "safety" guards in two consecutive BS1 sessions each
  created a worse failure than the one they prevented. When adding a guard, ask what the guard
  itself can break.
- **Any backbuffer-content detector must sample before our own writers.**
- ImGui widgets may only be called from the overlay's draw callback (render thread).
- **Probe hooks: the argument count must equal `ret imm / 4`.** A mismatch returns on a misaligned
  stack and pops a `Run-Time Check Failure #0 - ESP was not properly saved` dialog which **writes
  no crash dump** (RTC is a Debug compiler check, not an SEH fault). Press **Abort** on the dialog;
  never force-kill, which can leave the display mode unrestored. In Release the same mistake
  corrupts the stack silently. Disassemble and read the first `ret` before hooking anything new.
- No em dashes anywhere in this repo (PowerShell 5.1 parse errors and log/UI mojibake).

## PE identity (verified 2026-07-31, session 34, read-only)

| field | value | note |
|---|---|---|
| path | `D:\SteamLibrary\steamapps\common\BioShock Infinite\Binaries\Win32\BioShockInfinite.exe` | D: drive, same library as BS2 |
| machine | x86 `0x014C` | 32-bit, satisfies the repo's CMake guard |
| optional magic | `0x010B` (PE32) | |
| ImageBase | **`0x00400000`** | BS1/BS2 are `0x10900000` |
| DllCharacteristics | `0x8100` - **DYNAMIC_BASE (ASLR) NOT set**, NX set | **Loads at a fixed base.** BS1/BS2 are both ASLR-rebased. |
| LARGE_ADDRESS_AWARE | **yes** (`Characteristics 0x0122`) | So the 4 GB scan-range rule applies: heap/object scans must walk the full range, and any scan filter must exclude thread stacks/TEBs/PEBs explicitly. |
| SizeOfImage | `0x124F000` | |
| PE TimeDateStamp | `0x627BE455` = 2022-05-11 18:29:09 UTC | |
| CheckSum | `0x011590C3` | |
| file size | 18,368,840 bytes | |
| sha256 | `C20A42529C4181B1D10EED416D9847A823F9453DD267CA478EFB3A10014A0E77` | |
| Steam appid | **8870** | BS1 409710, BS2 409720 |
| sections | 6 | |

These five fields (TimeDateStamp / SizeOfImage / CheckSum / file size / sha256) are the **host
build fingerprint**. `core/util/module_id` gates object scans on it, exactly as BS1 does, because
every storefront ships a differently-linked exe and a mis-addressed scan on a wrong build is
silent. A wrong build must refuse object scans, keep the pattern-derived hooks, and stay playable.

**ASLR being off is a convenience, not a licence.** Keep using RVAs in `patterns.cpp`: it costs
nothing, matches the other two adapters, and survives the day a storefront ships a rebased build.

### Injection vector (verified, closed)

Import table parse of the shipped exe:

| DLL | how imported |
|---|---|
| `XINPUT1_3.dll` | **2 imports, both by ordinal: `#2` and `#3`.** Game IAT slot for ordinal 2 at RVA `0xCD4814`. |
| `d3d11.dll`, `dxgi.dll` | **not in the import table** - resolved dynamically at runtime |
| `binkw32.dll` | present (FMV playback) |

Consequences:

- **`src/proxy/` works verbatim.** Ordinals 2 and 3 are `XInputGetState` / `XInputSetState`, the
  same pair BS1 and BS2 import, and the existing `.def` ordinal table already covers them. This was
  the largest single injection risk on this game and it is closed before any code was written.
- **BS1's shipped IAT-hijack lane transfers directly.** On BS1 the proxy seam alone proved
  insufficient because the Steam overlay code-hooks the proxy's export thunk and eats
  `XInputGetState` before the proxy body runs; the fix was a last-hop IAT re-point keeping the
  previous target as passthrough. The IAT slot here is known (RVA `0xCD4814`). Expect the same
  Steam behaviour and plan for the same fix.
- **d3d11/dxgi are loaded dynamically**, so the mod must not assume `d3d11.dll` is resident at
  init time. `core/hooks/d3d11_hook`'s throwaway-device vtable grab does not depend on the game's
  imports and is unaffected, but the *ordering* in `framework::init()` needs a live check.
- The game selects its renderer at runtime (a D3D11 prerequisite installer ships in
  `Binaries\Prerequisites\D3D11Install_2010\`, and `d3d9.dll` is also referenced). **Confirm the
  live device is D3D11 in I0** before assuming the D3D11 hook path applies.

## UE3 reflection is intact (evidence, session 34)

ASCII strings present in `.rdata` (the UE3 FName pool stores names as plain ASCII), byte-scanned
from the disk image:

`PlayerController` · `UpdateRotation` · `PlayerCamera` · `CameraActor` · `SceneView` · `FOVAngle` ·
`GetPlayerViewPoint` · `MatineeCamera` · `ForceSkelUpdate` · `CheatManager` · `ConsoleCommand` ·
`XPlayerController` · `XHud` · `ToggleHUD` · `Scaleform` · `GFxMovie` · `PostProcessVolume`

UTF-16 strings present (UE3 `Exec` handlers compare wide literals via `ParseCommand`):
`SETRES` · `Stereoscopic3D` · `StartupMovie` · `FullScreenMovie` · `MotionBlur` · `Bloom` ·
`DepthOfField` · `HUD` · `SizeX`

Notably **absent**: `CalcView` (that is the Vengeance name - UE3 uses `GetPlayerViewPoint` /
`CalcCamera` / `Camera.UpdateViewTarget`), and `bStereo` / `StereoSeparation` / `EyeSeparation` /
`StereoDevice` / `StereoRendering` (see the stereo section).

**What this buys us.** The full `UObject` / `FName` / `UFunction` reflection system is present and
name-addressable. The seam to build against is **BS2's design, not BS1's**: hook
`UObject::ProcessEvent` plus `FindFunctionChecked`, filter by cached FName index, and mutate the
parameter block after calling the original. That is the standard UE3 anchor and it is far more
discoverable here than on either remaster.

**What may not carry.** BS1's single fastest instrument was the engine's name-based **native
function table** (1822 entries of `{ "int<Class>exec<Function>", impl, 0 }` in `.data`), which
resolved natives with zero hardcoded addresses and found the aim seam in minutes. **UE3 favours
indexed natives**, so budget for losing that lane and leaning harder on ProcessEvent-by-name plus
RTTI. Whether the table exists here is an I0/I2 question.

## Console, cheats and the Exec seam (the biggest single win over the remasters)

On BS1 and BS2 the in-game console is compiled out and key-bound commands are inert (verified live,
sessions 9 and 32). Everything had to go through calling the engine's `Exec` dispatchers directly.
**Infinite is different, and the evidence is the game's own shipped config.**

`XGame\Config\DefaultInput.ini` contains a live block, verbatim section header
`; --- Debug binds`, binding exec commands to keys in the retail build:

| key | command |
|---|---|
| `Delete` | `god` |
| `PageUp` | `ghost` |
| `PageDown` | `walk` |
| `End` | `preventdeath` |
| `Home` | `kydrawpathdata` |
| `Insert` | `debugaibehavior` |
| `Backslash` | `behindview` |
| `Backspace` | `viewclass XPawn` |
| `F1` / `F2` / `F3` | `viewmode wireframe` / `unlit` / `lit` |
| `F7` / `F8` | `set D3DRenderDevice bUsePostProcessEffects False` / `True` |
| `F9` | `shot` (screenshot) |
| `F10` | `dumpdynamicshadowstats` |
| `F11` | `remotecontrol` |

Two things follow, and they are worth a lot:

1. **`set <class> <property> <value>` is proven live by the game's own binds.** That is the UE3
   universal knob: any script property settable by name with no offset and no bitmask, and `set`
   writes the class default so the value survives load crossings. On BS1 this took until session 18
   to establish and needed a hand-built Exec seam.
2. **A test-loadout and cheat path exists without a working console**, which is what the user asked
   for. `god`, `ghost`, `walk`, `preventdeath` are directly bound; `viewmode` and the post-process
   toggle are directly useful as render instruments.

Additional surfaces found:

- `XGame\Config\DefaultEngine.ini`: `ConsoleClassName=XCore.XConsole`,
  `GameEngine=XCore.XGameEngine`, `GameViewportClientClassName=XCore.XGameViewportClient`.
- `DefaultInput.ini` `[Engine.Console]` carries a 20-entry `ManualAutoCompleteList` including
  `SaveCheckpoint`, `LoadCheckpoint`, `DisplayLastCheckpointInfo`, `ArchiveLastCheckpoint`,
  `GIVELOCKPICKS`, `Show STATICMESHES`, `obj garbage`, `DisplayVersion`, `simulateshippingbuild`.
  An autocomplete list is not proof the console *opens*, but it is proof these commands exist.
- `DefaultInput.ini` `[XCore.XConsole] TargetSwitchKey=Tab`. **No `ConsoleKey` / `TypeKey` is set**,
  which is likely why public guides report "no console" for this game.
- `XGame\Config\DefaultDesignerControlPresets.ini` holds a base64 `Data=` blob which decodes to a
  designer/debug action name list: `Ghost`, `Walk (Debug)`, `Prevent Death`, `God Mode`,
  `GiveAmmo`, `Slomo 10% / 50% / 100% / 200% / 1000%`, `QuickSave`, `QuickLoad`,
  `View Wireframe Mode`, `Viewmode Unlit` / `Lit`, `Draw Path Data`, `Debug AI Behavior`,
  `Post Process Effects`, `Navigation Pulse`, `Next Weapon`, `Next Plasmid`, `Swap Gear`,
  `Toggle Zoom`, `Debug_InvertY`, `DebugSkylineCone`. A second, independent debug surface.
  `Slomo`, `QuickSave` and `QuickLoad` are directly useful to the flat harness.

**Caveat, and it is the important one.** None of the above has been confirmed *live* yet. All of it
is read out of the shipped config files and the exe's string tables. Per the standing rule, a
config entry is a claim, not an effect. **I0 verifies each by observable behaviour** (press
`Delete`, take damage, survive) and records the result here - including any that turn out inert.

## Renderer, resolution and FOV (config-level findings, unverified live)

### Frame pacing levers

| setting | file:line | shipped value | why it matters |
|---|---|---|---|
| `OneFrameThreadLag` | `Engine\Config\BaseEngine.ini:726` | `True` | Setting this `False` removes one frame of render-thread lag. This is a **config-level analogue of BS1's `reentry 1t`**, obtained without hooking a flush point. If it works, it deletes the single most expensive rabbit hole on BS1 (sessions 5-8). |
| `bSmoothFrameRate` | `BaseEngine.ini:192` | `TRUE` | Frame smoothing must be off for VR. |
| `MinSmoothedFrameRate` / `MaxSmoothedFrameRate` | `BaseEngine.ini:193-194` | 22 / 124 | |
| `RenderThreadJobQueuePriority` | `XGame\Config\DefaultEngine.ini:456` | 7 | |
| `AllowD3D11` | `BaseEngine.ini:733` | `True` | |

### Resolution

- `XGame\Config\DefaultUserOptions.ini`: `DefaultResolutionX` / `DefaultResolutionY` = `-1`
  ("use the current system settings default, will be saved out as its own version after that
  point"), `DefaultDisplayMode=1` (0 windowed / 1 fullscreen / 2 fullscreen-windowed).
- `XGame\Config\DefaultEngine.ini:219-220`: `ResX=1280`, `ResY=720`.
- `SETRES` exists as a UTF-16 exec string in the exe. On BS1 the equivalent **faulted** through the
  viewport Exec seam, which is why the ini lane became primary there. Test it here; do not assume
  either way.
- User config is written to `%USERPROFILE%\Documents\My Games\BioShock Infinite\XGame\Config\`
  (standard UE3). That directory **does not exist yet** - the game has never been launched on this
  machine. It appears after the first flat run.
- `CommonAspectRatios` lists 16:10, 16:9, 4:3 and three portrait ratios, with
  `CommonAspectRatioGroupingSlop=0.11`. The presence of portrait ratios suggests the options UI
  will *group* an odd resolution rather than reject it, which is promising for the near-square
  render the headset wants.

**Acceptance for any resolution write is the backbuffer at first Present after a relaunch.**
BS2 taught this the hard way: the BS1-shaped port wrote its keys, re-read them, logged
`verified`, and the engine rendered 1920x1080 anyway.

### FOV

- `XGame\Config\DefaultEngine.ini:431`: `FOVAngle=70.000000` (under `[UnrealEd.EditorEngine]`);
  `Engine\Config\BaseEngine.ini:415`: `FOVAngle=90.000000`.
- `XGame\Config\DefaultUserOptions.ini` has a **native FOV slider**, and documents its own formula:
  `adjusted in-game FOV = (MaxUserFOVOffsetPercent*0.01f) * <current base in-game FOV> * FieldOfView`
  with `MaxUserFOVOffsetPercent=15.0f` and `DefaultFieldOfView=0` (range 0 to 1).
- So the native slider spans roughly **70 to 80.5 degrees**. That is a real native lever, and it is
  more than BS1 had at the start, but it is **not enough for VR** on its own. A lever is still
  needed. The property chain is named (`FOVAngle`, `FieldOfView`, `MaxUserFOVOffsetPercent`), which
  makes `set`-by-name the first thing to try rather than a memory scan.
- `FOVAngle` also appears in the exe's FName pool, so it is a live script property, not only an ini
  key.

**Do not tune anything against a single aspect.** Build the lens decoder first (see the roadmap's
I5), sample strided, vote, and derive the law from two different backbuffer aspects.

### Stereo: expect to build it, do not expect to inherit it

Infinite shipped a stereoscopic 3D mode:

- `XGame\Config\DefaultEngine.ini:789` `[Stereoscopic3D]` with
  `bShowStereoscopic3DInGraphicsOptionsMenu=false`.
- `XGame\Config\DefaultUserOptions.ini:96` `DefaultStereoscopic3DAdjust=0` ("same behavior as
  DefaultFieldOfView, but for Stereo3D strength").
- `Engine\Config\BaseEngine.ini:199` `AllowNvidiaStereo3d=False`.
- `Stereoscopic3D` appears in the exe as a UTF-16 string (consistent with being an ini section
  name).

**But** no `bStereo`, `StereoSeparation`, `EyeSeparation`, `StereoDevice` or `StereoRendering`
names appear anywhere in the exe. That pattern points at a **driver-side path (NVIDIA 3D Vision via
nvapi)** rather than an engine per-eye render loop we could drive.

Note also that praydog's UEVR - which is **all-rights-reserved, concepts only, zero code** - works
by hijacking UE4's `IStereoRendering`/`FFakeStereoRendering`. That interface is a UE4-era
construct; whether this UE3 build has an ancestor of it is unknown.

**Plan for SequentialReentry.** Timebox the native-stereo check to one session slice in I2. If it
exists it deletes an entire milestone; if it does not, record the negative here with the evidence
and move on. Do not let it become a rabbit hole.

## HUD and cinematics: two structural differences from the remasters

- **UI is Scaleform GFx** (`Scaleform`, `GFxMovie`, `XHud` in the FName pool), not the embedded
  gameswf the remasters use. `core/gfx/hud_capture` is therefore a **worked example, not a
  library**: its whole fingerprint is a contiguous run of non-indexed draws on the tonemap target
  carrying a Vengeance-specific gameswf batch-flush RVA in every callstack, plus a garbage
  destination-alpha repair pass specific to gameswf. Expect to build a new classifier. It may be
  *cleaner*: a GFx movie often renders to its own target, which could be captured directly rather
  than classified out of a batch.
- **Cinematics are two distinct classes**, unlike BS1:
  1. **Bink FMV** through `binkw32.dll` - 100+ `.bik` files in `XGame\Movies` (`BSI_AttractMovie`,
     `BioshockInfinite_Credits`, the voxophone/PSA reels, per-Vigor tutorials).
  2. **Engine-rendered Matinee** - `MatineeCamera` is in the FName pool.
  They will need different handling. `[FullScreenMovie]` sections exist in both engine inis.
- Infinite is a **deferred** renderer with an Irrational-custom replacement. The BS1/BS2
  forward-renderer frame fingerprints do not apply, and the cb0 screen-ray block offset (BS1 = 12,
  BS2 = 16) must be re-derived. `tools/decode-framedump.ps1 -ScanLayout` brute-forces the offset
  with structural validation, and `-Diff <other> -DiffFovs a,b` finds the floats whose ratio
  matches `tan(a/2)/tan(b/2)` while assuming nothing about layout. That instrument is what cracked
  BS2; use it here.

## Input: every control is a named exec command

`XGame\Config\DefaultInput.ini` defines the full binding set as UE3
`+Bindings=(Name="<key>",Command="<exec>")` pairs. This is a large advantage over BS1, where
`exec NextWeapon` faulted and weapon switching could not be driven flat at all.

Axes: `aLeftStickX` / `aLeftStickY` / `aRightStickX` / `aRightStickY` (gamepad),
`aBaseX` / `aBaseY` / `aStrafe` / `aLookUp` / `aUp` / `aMouseX` / `aMouseY` (general).

Actions seen: `StartFireWeapon` / `StopFireWeapon`, `StartFirePlasmid` / `StopFirePlasmid`,
`NextWeapon`, `NextPlasmid`, `SwapWeapon`, `SwapPlasmid`, `ReloadOrHoldToHackOrUse`,
`XDisengagePlasmidOrReloadWeapon`, `ToggleCrouch`, `Jump`, `StartSprint`, `XToggleZoom`,
`ShowPauseMenu`, `XUserInterface_OnBackButtonPressed`, `XNavShowPulse`,
`XNavQuickToggleCycleLeft` / `Right`, `XMakeUnstableSelection`, `FlashCommand <name>`
(the Scaleform bridge - `AbortHack`, `AutoHack`, `BuyoutHack`, `ActivateFirstNeedle`, ...).

**The Skyline is its own control family.** The ini documents an `XInputHandler` chain mechanism
(commands joined by `+`, each short-circuiting the rest on success) with a large TBar set:
`XJumpOffTBar`, `XPerformTBarToTBarLookAtTransfer`, `XPerformTBarToGroundTransfer`,
`XPerformGroundToTBarTransfer`, `XPerformTBarBoost`, `XPerformTBarDodge`,
`XPerformTBarMeleeTransfer`, `XStartTBarZoom` / `XStopTBarZoom`, and the matching
`XActivate*/XDeactivate*HighlightEffect` pairs. Note the game's own comment: the first item in a
chain must be an `XInputHandler`, only handlers may appear until the last entry, and a handler
returning true short-circuits the rest. Any VR remap must preserve that chain semantics.

Also present: `bInvertMouse`, `bEnableMouseSmoothing=false`, `LookUpScale=250`,
`[XCore.XPlayerInput] bIgnoreWindowsMouseSensitivity=false`.

## DLC

All three story/arena DLC are installed and are **in scope for tuning** (user directive,
session 34): bring-up happens on the base campaign, but aim, scale, HUD and viewmodel calibration
must hold in all of them.

| folder | size | content |
|---|---|---|
| `DLC\DLCA` | 5.8 GB | (Clash in the Clouds / Burial at Sea - map to titles in I0) |
| `DLC\DLCB` | 7.5 GB | |
| `DLC\DLCC` | 11.8 GB | |
| `BirdsEye`, `ChinaBroom`, `IndustrialRevolution`, `SeasonPass`, `UpgradePack` | ~1 MB each | entitlement stubs |

Burial at Sea adds weapons and a Vigor (Old Man Winter) the base game does not have, so per-weapon
aim profiles must cover them.

---

# Derivation recipes

Short form. The full versions, with worked examples and the traps, are in
[../bioshock1/ENGINE_NOTES.md](../bioshock1/ENGINE_NOTES.md) and
[../bioshock2/ENGINE_NOTES.md](../bioshock2/ENGINE_NOTES.md).

- **Static caller census - run this BEFORE hooking anything.** For any absolute-addressed function,
  scan the exe for `E8` opcodes whose rel32 lands on it. Zero callers on a function the engine
  "must" call every frame means the dispatch is inlined or dynamic. This is the check that cracked
  BS2's dead-thunk mystery after a hook was installed that never fired.
- **FName-chain event scan** (`core/hooks/pattern_scan`, game-agnostic): name string -> imm32 xref
  -> forward to the `E8` FName-ctor call -> past it to the `89 0D` store = cached index global ->
  global xrefs (minus the init site +/-200 bytes) -> backward prologue walk `CC CC CC 55 8B EC`.
  Caveat: the prologue heuristic **misses frameless functions**. And the first prologue-valid
  candidate is not necessarily a *live* function - check callers.
- **RTTI walk**: find the `.?AVClassName@@` TypeDescriptor -> the dword referencing its VA is
  COL+12 -> the dword referencing the COL's VA is vtable-4. Validate the method by reproducing
  known-good vtables before trusting a new one. **Verify RTTI is present in this build first** - if
  it is stripped, this whole lane dies and `GObjObjects` enumeration replaces it.
- **Value scan through the command seam**: `memscan`/`memscani` -> have the value changed through
  the game's own UI -> `memrescan` -> poke each survivor + screenshot + `img-diff` to find the
  *consumed* copy -> `memptr` for the owning static root. Two traps: an ini value may be an int
  (float scans are blind to it), and a coincidental range hit can look like a static root.
  `fsweep` covers values with no known representation.
- **Scan hygiene**: this exe is LAA, so any heap/object scan walks the full 4 GB. Exclude thread
  stacks, TEBs and PEBs explicitly - they are `MEM_PRIVATE | PAGE_READWRITE`, exactly like the
  heap, and BS1's scanner spent 3.8 s of blocked game thread finding **its own argument spill**.
  Use `core/hooks/heap_scan` (HeapWalk fast path, XOR-masked needle, sliced fallback, fail-closed
  on ambiguity). No logging or allocation inside an SEH guard: MSVC under `/EHsc` does not run
  destructors during SEH unwinding, so a fault taken while the log mutex was held left it held for
  the life of the process.
- **Frame map**: the in-tree `dumpframe` / `dumpframe full [N]` (`core/gfx/frame_inspector`), then
  `tools/decode-framedump.ps1` offline. RenderDoc has never been needed on this project.
- **UnrealScript decompile**: `tools/uscript/` (gitignored) drives UE Explorer's portable
  `Eliot.UELib.dll` from PowerShell. UELib **explicitly supports Infinite (package version 6829)**,
  so this lane ports directly. Fallback when deserialization fails: read the raw source text
  embedded in the package, checking **both UTF-16 alignments** (a name at an odd byte offset is
  invisible to an even-aligned decode).

---

# Hook inventory

Nothing derived yet. Each row gets filled in with its RVA and its derivation method as it lands,
and every value here must also exist in `src/game/bioshockinf/patterns.h/.cpp` and nowhere else.

| Hook | RVA / locator | Purpose | Derivation | Status |
|---|---|---|---|---|
| `IDXGISwapChain::Present` | vtable slot | frame boundary, XR pacing, overlay, mirror | kiero-style throwaway device (core, game-agnostic) | pending I1 |
| `IDXGISwapChain::ResizeBuffers` | vtable slot | RT cache invalidation | same | pending I1 |
| `UObject::ProcessEvent` | - | the universal UE3 seam: camera, command poll tick | vtable slot off a known object + prologue gate | pending I2 |
| `UObject::FindFunctionChecked` | - | cache the `UFunction*` for the camera event by FName index | FName-chain scan + caller census | pending I2 |
| camera event (`GetPlayerViewPoint` / `CalcCamera` / `UpdateViewTarget`) | - | 6DoF camera override | ProcessEvent filter by cached FName index | pending I2 |
| scene build / draw root | - | SequentialReentry stereo seam | frame inspector callstack RVAs + capstone | pending I2/I6 |
| `XInputGetState` | game IAT RVA `0xCD4814` (XINPUT1_3 ord 2) | synthetic gamepad | import table parse (**done**, session 34) | confirmed, unhooked |
| Draw / DrawIndexed / OMSetRenderTargets / Map+Unmap | context vtable | HUD classification, frame dumps, lens watch | core `frame_inspector` | pending I2 |
| engine `Exec` dispatchers | - | console commands by code | RTTI + vtable walk | pending I2 |

# Symbol / offset table

Nothing derived yet. Format matches the sibling files: symbol, RVA or offset, type, derivation
method, session.

# Dead ends

Recorded here as they happen, with the address, so nobody re-walks them. BS1's list saved real
time; start this one early.

- (none yet)
