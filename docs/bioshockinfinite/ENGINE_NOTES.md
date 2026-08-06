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
  `Binaries\Prerequisites\D3D11Install_2010\`, and `d3d9.dll` is also referenced).
  **CONFIRMED LIVE, session 34: the renderer is D3D11.**

### Live process facts (session 34, game running, no mod)

Module list read with **32-bit** PowerShell - this matters: a 64-bit host enumerating a 32-bit
process sees only the WOW64 shim layer (`ntdll`, `wow64*`), and both `Get-Process().Modules` and
`tasklist /m` return a useless 6-module answer. Use `SysWOW64\WindowsPowerShell\v1.0\powershell.exe`.
123 modules total.

| module | state | meaning |
|---|---|---|
| `d3d11.dll`, `DXGI.dll` | LOADED | the D3D11 path is live |
| **`nvwgf2um.dll`** | LOADED | NVIDIA's **DX10/11** user-mode driver. `nvd3dum.dll` (the DX9 UMD) is **absent** - this is the decisive evidence, since `d3d9.dll` alone proves nothing (the launcher and Bink pull it in) |
| `d3dx11_43.dll`, `d3dx9_43.dll` | LOADED | helper libs, both present regardless |
| **`XINPUT1_3.dll`** | LOADED | the injection vector is live at runtime, not merely an import-table entry |
| **`GameOverlayRenderer.dll`** | LOADED | **the Steam overlay is present.** This is what code-hooks the proxy's export thunk on BS1 and eats `XInputGetState` before the proxy body runs. Expect to need the IAT-hijack lane (slot RVA `0xCD4814`), not the proxy seam alone. |
| `nvapi.dll` | LOADED | consistent with the read that "Stereoscopic3D" is driver-side 3D Vision rather than an engine per-eye path |
| `binkw32.dll`, `PhysX3_x86.dll` | LOADED | FMV and physics as expected |
| `d3dcompiler_43.dll` | absent | shaders are precompiled; `core/gfx/blit`'s lazy `D3DCompile` path will pull it in itself |

**Window geometry:** `2566 x 1469` outer, so a **2560x1440 client** - matching `XEngine.ini`
`ResX`/`ResY` exactly, and windowed (title bar present, `DisplayMode=0`). Note this is *not* yet
backbuffer-at-first-Present acceptance for DR-I8; it is the window, which is a strong hint and no
more.

**Harness verified against the live process** (no mod installed):

- `game-shot.ps1 -Game bsi` captures **real D3D content** via `PrintWindow` +
  `PW_RENDERFULLCONTENT` - not a black frame, which was not guaranteed on a D3D11 title. The
  conflict guard passed, and the capture is usable for `img-diff` A/B work from day one.
- `game-cmd.ps1 -Game bsi` passed the guard, created `%LOCALAPPDATA%\BioshockVR\bsi\` and wrote
  `command.txt` with no BOM. Nothing reads it yet - that arrives with I1's core poller.
- The conflict guard was exercised in **both** directions: it refused while BS2 was running
  (pid 24588, and again 22136) and allowed once BS2 closed. Positive and negative control both
  observed.

## First code inside the process (session 35, I1 - LIVE, and it changes the confidence table)

Everything above this line was derived from the disk image or observed from outside. This section is
the first set of facts measured **from inside `BioShockInfinite.exe`** by our own DLL.

### Injection and identity

| fact | evidence |
|---|---|
| **The `xinput1_3.dll` proxy works verbatim** | the mod ran at all. `input: bridge registered with proxy seam (module 74B30000)`. No change to `src/proxy/` was needed, exactly as the import-table parse predicted. |
| **Host build fingerprint matches on all four fields** | `pe-timestamp 0x627BE455 size-of-image 0x0124F000 checksum 0x011590C3 file-bytes 18368840`, read from the live PE headers. Note Infinite's exe **does** carry a real checksum, unlike BS1's. |
| **ImageBase 0x00400000 confirmed live** | `[bsi] main module: base 00400000 size 0x124F000`. ASLR really is off. RVAs are still what `patterns.h` stores. |
| **The Steam overlay does not block injection** | `GameOverlayRenderer.dll` is loaded (session 34) and the mod still loaded and ran: the proxy pulls `bioshockvr.dll` in from its own `DllMain`, long before anything calls `XInputGetState`. Whether the overlay eats the *input* thunk is still open and belongs to I7. |
| **`CSERHelper.dll` displaces our unhandled-exception filter here too** | `crash: our exception filter had been displaced by CSERHelper.dll+0x12571 - re-armed (chaining to it)` at the first Present. 2K's crash reporter behaves exactly as it does on the remasters, so core's periodic re-arm is load-bearing on this game as well. |

### The renderer, from inside

`d3d11.dll` and `dxgi.dll` being **absent from Infinite's import table changes nothing** for us, and
the reason is worth writing down so it is not re-investigated: `bioshockvr.dll` links `d3d11` itself,
so *our* import table loads it before any of our code runs, and `d3d11_hook`'s throwaway device pulls
DXGI in turn. The swapchain vtable is process-wide, so the game's later-created swapchain dispatches
straight into our detour. Ordering verified live - hooks installed at T+0.4 s, first Present at
T+8.5 s, no gap and no retry needed.

| measured at the first Present | value |
|---|---|
| backbuffer | **2560 x 1440**, `DXGI_FORMAT_R8G8B8A8_UNORM` (28), `Windowed=1` |
| feature level | `0xB000` = **D3D_FEATURE_LEVEL_11_0** |
| adapter | NVIDIA GeForce RTX 4060 |
| frame inspector | **15/15 context vtable slots hooked** on the game's own immediate context |
| one lite frame dump | 482 events, 68 resources |
| lifetime draw census after ~22k presents | DrawIndexed 7,002,643 · Draw 524,784 · DrawIndexedInstanced 807,738 · DrawInstanced 0 · SetRenderTargets 1,409,381 · ClearRTV 398,619 · ClearDSV 225,272 |

**A free half-answer for DR-I8.** The live `XEngine.ini` says `ResX=2560` / `ResY=1440`, and the
backbuffer at first Present is 2560x1440. So Infinite's config resolution **is** honoured by the
renderer - which is more than BS2 could say for its viewport keys. This does **not** yet prove that
*writing* a new value lands; that still needs a write plus a relaunch, and acceptance is still the
backbuffer, never the read-back.

**Observation for the presentation milestone (I9 post-restructure), recorded now so it is not mistaken for a bug later:** core's letterbox
detector (BS1-tuned) fires on Infinite's loading/cinematic bars and produces incoherent readings
(`top 1440 px, bottom 0 px of 1440`). The detector needs re-deriving for this game; nothing consumes
it yet.

### The camera seam, probed read-only (still not hooked)

`patterns::resolve` reads the first bytes at the two session-34 RVAs and logs them. No hook, no
write, no scan - this only promotes "the disk image says there is a function here" to "the live image
has these bytes here":

| RVA | live VA | first bytes | reading |
|---|---|---|---|
| `0x1E10C0` (impl) | `0x005E10C0` | `55 8B EC 83 E4 F0 81 EC A4 00 00 00 53 56 8B 75` | `push ebp; mov ebp,esp; and esp,-0x10; sub esp,0xA4; push ebx; push esi; mov esi,[ebp+..]` - an **aligned-stack MSVC prologue**, which is exactly what a function containing the documented 4x4 SSE transform should have. |
| `0x129280` (exec thunk) | `0x00529280` | `83 EC 1C 56 8B F1 8D 44` | `sub esp,0x1C; push esi; mov esi,ecx` - **frameless**. Independently confirms the recipe caveat that the `CC CC CC 55 8B EC` prologue heuristic cannot find functions like this one. |

Still unproven: that the implementation is called with the shape we think, and that hooking it is
safe. That is DR-I2.

## DR-I1: UE3 reflection - PASS (session 36, offline)

`UObject::ProcessEvent`, `UObject::FindFunctionChecked` and the ProcessEvent vtable slot are all
derived, with two independent instruments agreeing and a caller census whose every prediction held.
**No game was running for any of this** - it is all disk-image analysis, which is what made it the
right work to do while BioShock 2 held the machine.

| symbol | RVA / locator | signature | derivation |
|---|---|---|---|
| **`UObject::ProcessEvent`** | **`0xCFE70`** | thiscall, **3 stack args, `ret 0xC`** | vtable slot `+0x7C` read from 1582 candidate vtables: **1038 votes**. 672 bytes, single `ret 0xC`, `int3`-padded. |
| **`AActor::ProcessEvent`** | **`0x19A150`** | thiscall, `ret 0xC` | the same histogram's **runner-up, 175 votes**. A thin override: two global checks, `test [esi+8],0x200`, then `call 0x4CFE70`. |
| **`UObject::FindFunctionChecked`** | **`0xD1090`** | thiscall, **3 stack args, `ret 0xC`** | UTF-16 `Failed to find function` at `.rdata 0xD07CC0`; its only code xref `0xD1131` resolves to this function. 304 bytes, single `ret 0xC`. |
| **`UObject::FindFunction`** | **vtable slot `+0x54`, impl RVA `0xD1030`** | virtual; thiscall, 3 stack args (`FName{Index,Number}` + `UBOOL Global`), `ret 0xC` like FFC | slot read out of FindFunctionChecked's own body: `mov eax,[esi]; mov edx,[eax+0x54]; call edx`, then the null test that reaches the `appErrorf`. RVA derived twice in agreement: the function immediately preceding FFC's `0xD1090` in .text starts at `0xD1030`, and the live session-36 vtable read of a latched APlayerController put `0xD1030` in slot `+0x54`. Consumed by `bsicall` (session 37) as its dispatch interlock: the live slot must equal this RVA or the call refuses. |
| **ProcessEvent vtable slot** | **`+0x7C`** (index 31) | | **407 of 420** decodable callers of FindFunctionChecked agree. |

### Why this is believed, rather than merely plausible

**The census predictions were written down before the run** (that is what makes it an instrument
and not a readout) **and every one held**:

| target | predicted E8 callers | actual | predicted abs dword refs | actual |
|---|---|---|---|---|
| `GetPlayerViewPoint` impl `0x1E10C0` | 14 | **14** | small | **0** |
| every exec thunk checked | 0 | **0** | 1-2 | 2 |
| `FindFunctionChecked` | many (>= 100) | **426** | ~0 | **0** |
| `UObject::ProcessEvent` | few (virtually dispatched) | **1** | hundreds | **2144 in .rdata** |
| `AActor::ProcessEvent` | few, fewer refs than the base | **0** | many but fewer | **268** |

Two further corroborations that were not predicted and are therefore worth more:

- ProcessEvent's **single** E8 caller is `0x19A189` - which is exactly the `call 0x4cfe70` inside
  `AActor::ProcessEvent`. Nothing else in 10 MB of `.text` calls it directly.
- **The base/override split is settled by structure, not by vote count**: the runner-up tail-calls
  the mode. A histogram alone could never have told us which of the top two is the base class, and
  this is the one thing the plan flagged as needing a separate method.

`GetPlayerViewPoint` having **0** absolute dword references is also a real finding: it is not in
any vtable, so it is a non-virtual member reachable only by address. That closes off a
"hook it through a vtable instead" alternative before anyone spends a session on it.

### The two methods worth reusing

**1. The E8-target set replaces every prologue heuristic.** Collect every `E8` call target in
`.text` - 38,638 of them - and that set *is* the list of known function entry points. "The function
containing address X" becomes "greatest entry <= X" by binary search. No `CC CC CC 55 8B EC`
backward walk, and therefore none of its silent failure mode: on this exe that walk returns the
**previous** function rather than failing, because the exec thunk at `0x129280` is frameless
(`83 EC 1C 56 8B F1`). This is why the bsi lane must never call
`pattern_scan::find_event_function`, and the header now says so.

**2. MSVC hoists the vtable pointer, and that broke the first attempt.** A UE3 generated event stub
reads:

```
mov edi, [esi]              ; vtable, hoisted well before it is used
...
call FindFunctionChecked    ; E8
mov edx, [edi+0x7C]         ; the slot
call edx                    ; ProcessEvent
```

Searching for `call [reg+disp]` (`FF /2`) finds **0 of 426**. A decoder must track register loads
and resolve `call reg` through them. The first pass of this analysis reported a nonsense negative
disp8 for exactly this reason, and it was only caught by disassembling a caller instead of trusting
the histogram.

### CORRECTION to the table below: three "impl RVA" rows are exec thunks

`APlayerController::ConsoleCommand 0x136070`, `AXPawn::SetWeapon 0x4F9ED0` and
`AXWeapon::AddAmmo 0x5017D0` were recorded in session 34 as implementation RVAs. The caller census
says otherwise: **all three have 0 `E8` callers** and a handful of absolute dword references, which
is the exact signature of an exec thunk. The native table's second dword is always the thunk, never
the C++ implementation.

Consequence for the I2 test-loadout path: it cannot be "call these three addresses". A thunk *is*
callable, but its signature is `execFoo(FFrame& Stack, void* Result)` and calling it means building
an `FFrame`. **The better route now exists and needs no new addresses at all**: go through
`ProcessEvent` by name, which is BS2's design.

Incidental: a thunk carries exactly **2** absolute references (its native-table entry plus one
more), so `ConsoleCommand`'s **6** and `SetWeapon`'s **4** mean those names are registered on
several classes - `AActor`, `APlayerController`, `AXPlayerController` and `UGameViewportClient` all
register `ConsoleCommand`, which is 4 of the 6.

### Recorded negatives (do not re-hunt these)

Absent from the exe in **both** ASCII and UTF-16: `Runaway loop detected`, `Infinite script
recursion`, `Missing native function`, `Stack overflow`, `ProcessEvent`, `FindFunctionChecked`,
`ProcessInternal`, `CallFunction`, `ProcessRemoteFunction`. Absence is **not** evidence of removal -
a shipping build folds or strips most `appErrorf` format strings. Only two anchors survive the link:
`Failed to find function` (the one that worked) and `Accessed None` (UTF-16, inside the function at
`0xD80B0`, 1 caller - the script VM's null-property path).

## LIVE RESULTS (session 44 - I7: the per-game pad map, Touch bindings, aim)

### The pad-map seam, and the BS1 inertness proof that gates it

The XR-to-pad table is now per game: `bvr::input::PadProfile` (one atomic in the
bridge, default `Bioshock1`) selects between two `constexpr PadMap` tables next to the
composer in `core/vr/openxr_input.cpp`. Design rationale and the rejected alternative:
ARCHITECTURE decision log, 2026-08-06.

**The BS1 proof, measured on the sim lane before a line of Infinite work** (the banked
`claimRatioH` is a stereo-geometry number and cannot see an input regression, so it is
the "nothing else moved" control here, not the proof). Boot to gameplay via
`boot.ps1 -Attach`, `vrinput on`, `vrinput padlog on`, then a per-control sweep:

| control | composed | banked BS1 expectation |
|---|---|---|
| Touch A / B / X / Y | `0x1000 A` / `0x8000 Y` / `0x4000 X` / `0x2000 B` | the s19 B->Y, Y->B re-route SURVIVES |
| LS click | `0x0040 LS` | forwarded |
| RS click | **no pad edge** | eaten - it is the ammo modifier, never a binding |
| grip L 0.60 / 0.75 / 0.60 / 0.50 | none / `0x0100 LB` / none (holds) / release | the 0.70 press / 0.55 release hysteresis, exactly |
| grip R 1.0 | `0x0200 RB` | forwarded |
| trigger L / R | `lt=255` / `rt=255` | analog passthrough |
| menu tap / hold >= 500 ms | `0x0010 START` / `0x0020 BACK` | tap-pulse and hold |
| thumbrest + flick up / down / left | `0x0001 DU` / `0x0002 DD` / `0x0004 DL` | the three ammo slots |
| thumbrest + flick RIGHT | **no pad edge** | BS1 has no fourth direction, and gains none |

Turn suppression proved by an A-B pair rather than assumed: right stick at full
deflection with a concurrent A press reads `rx=32767` with no modifier and `rx=0` with
the thumbrest modifier held. Left stick forward reads `ly=+32767`. Controls: stereo
capture `claimRatioH 1.01769` == the banked 1.018, sim `errors 0`, zero mod faults,
zero crash dumps, and **zero `pad profile` lines in BS1's log** - the setter is never
called there, which is the direct evidence that BS1 took the default arm.

## LIVE RESULTS (session 43 - the stutter hunt: spike instrument, flat repro)

### The spike class REPRODUCES FLAT at native 2064x2208 (pre-instrument baseline)

First flat measurement (TWN2 save, indoors at the Blue Ribbon, sim at 80 Hz to match
the VDXR headset run, stereo + preset armed, s42 TRACE pairs instrument):

- **Static 60 s**: mean pinned at 12.5 ms (= period), sd ~1.1 ms, max 14.7-15.8 ms -
  ZERO spikes. Steady-state is as clean flat as it was in the headset.
- **`head orbit 120 6000`** (120 deg/s yaw sweep, both directions): mean rises to
  13.7-14.3 ms in the sweep seconds (render cost above budget - the pair rate dips
  below refresh), max 16.6 ms - still no spike-class intervals.
- **`head orbit 240 6000`**: ONE spike-class interval - **max 36.8 ms (~3 dropped
  frames) mid-sweep** (15:12:58, sd exploded 1.2 -> 2.9 ms in that second), the rest
  of the sweep seconds looking like the 120 case.

READ: the hitch class is reproducible flat, is view-change-bound (only during fast
yaw), and is much milder INDOORS than the user's outdoor headset bursts (39-113 ms
every few seconds) - consistent with the load-sensitivity diagnostic. The hunt can
iterate flat; outdoor repro needs the pad lane or an outdoor checkpoint.

### WALKING is the real flat trigger, and every spike is OUTSIDE our code

The pad lane moves the pawn in the TWN2 save (30 s stick-forward = 1770 UU; the s42
"scene-locked" observation was that probe save's scripted state, not a lane defect).
A 100 s turn-and-walk wander at native res reproduced the HEADSET SIGNATURE flat:
7 spikes (29-350 ms, bursts, sd exploding to 47 ms in the bad seconds). The s43
pair-close snapshot's verdict on every one: `unattributed` carries 27-340 ms while
our two detour halves carry <= 12 ms (one normal gated wait) - the stall is the
GAME side, never capture/submit/pacing. The SR lane is exonerated as the carrier.

### The spike-in-progress sampler names two stall signatures (s43)

The escalation instrument (4 ms poller; when the open pair's age exceeds 2.5x period
it stack-captures the draw thread + all threads via the s34 watchdog machinery, once
per episode, 2 s rate-limit, 40/session cap - `SPIKE-SAMPLE` lines in pacetrace.log):

- **Signature A - the 30-second grid.** Samples land on an EXACT 30 s cadence
  (:16/:46 wall-clock marks, idle or wandering alike), ~30-50 ms stalls. Mid-stall
  the game thread (draw tid) sits in ntdll at a wait, through a repeated exe chain
  (ret RVAs 0x4BA248 <- 0x4BCE6D <- 0x4BEBCA, plus 0xC47A70/0xC47F90/0xC4812D and
  0xBC712B frames): the 0x4BA248 call site is `mov eax,[edx+0x10]; push 0x64;
  call eax` - a virtual event-Wait(100 ms) in a loop (FEvent::Wait shape). EVERY
  other thread is also parked in ntdll waits at that moment - nobody is computing;
  the game thread is waiting on a barrier. 30 s == this build's
  `TimeBetweenPurgingPendingKillObjects=30` ([Engine.Engine]; XEngine.ini:98,
  regenerated from DefaultEngine.ini:300) - the GC tick's flush barrier is the
  prime suspect (UE3 GC flushes async loading before collecting; the wait-with-
  timeout loop matches a flush, not the CPU mark phase). CONFIRMATION PROBE below.
- **Signature B - off-grid, view/traversal-bound.** Mid-stall the game thread is in
  ntdll under a DEEP REPETITIVE chain (ret RVAs 0xA7F3C5/0xC30928/0xAA2083/0xAA217E/
  0xD7C5CC repeating, entered via 0x4639F3, 0xC42400) - an iterate-and-wait shape:
  0xAA217E's call target 0xA7F1E0 walks an object array testing flag words and
  calling two virtuals per element (streaming/level-visibility update shape), and
  the thread ends in a wait inside that walk (IO?). This is the streaming-class
  stall the head-turn/walking trigger produces. Named by shape, not yet by string -
  further naming deferred unless the levers miss.

### THE CAUSE, NAMED AND CONFIRMED BY A-B-A: the 30-second GC tick (s43)

`TimeBetweenPurgingPendingKillObjects` intervention, same save, same sim (80 Hz),
native 2064x2208, matched protocols:

| leg | interval | idle window | wander (5 rounds turn+walk) |
|---|---|---|---|
| A (boots 3-4) | 30 | spike/sample grid at EXACT 30 s marks (:16/:46), 29-50 ms stalls | 4-7 spikes (29-350 ms, bursts) |
| B (boot 5) | 300 | grid GONE for 3 min (one post-load settle burst ~60 s after load) | **0 spikes** |
| A' (boot 6, reversal) | 30 | periodic stalls RETURN - 39.9 + 33.6 ms spikes plus a 16.6 ms sub-threshold tick at ~30/60 s spacing | - |

Nuance recorded honestly: the tick's cost VARIES with accumulated garbage - on a
fresh-loaded idle scene some ticks stay under the 25 ms spike threshold (the A' leg),
while during traversal they stack with streaming work into the 40-350 ms bursts the
headset feels. Mechanism fit: UE3's timed full GC flushes async loading first (the
Signature-A event-wait barrier), and its cost scales with object/garbage churn -
exactly the "worse outdoors / worse while moving" load sensitivity.

**The candidate fix (headset verdict pending): `TimeBetweenPurgingPendingKillObjects=300`
in the game folder's DefaultEngine.ini** (backup `.bvr-bak-s43` beside it). Risk notes:
UE3 still GCs on level transitions regardless of the timer, so garbage does not grow
unbounded; a 300 s timed GC will cost more when it does fire (rare enough to be
acceptable if the headset agrees); 32-bit address headroom is the watch item on long
sessions. Signature B (traversal/streaming walk) remains the documented residual with
the texture-pool lane researched and ranked next.

### The "jumpy camera": SOLVED - pose attribution lag is TWO on this engine (s43b, headset-verified)

**VERDICT (user, 2026-08-06, same night): lag 2 is "perfect - everything is extremely
smooth."** The A/B named the pipeline depth by intervention: Infinite's threaded
one-frame-lag renderer presents content TWO locate generations old; the historical
one-back attribution (BS1's lockstep calibration) left a one-period pose error that
scaled with head speed - the reported wobble. The adapter now ships
`set_pose_lag(2)` at init (bioshockinf_adapter.cpp); the F10 radios and `bsipose`
stay live for re-derivation if the substrate ever changes (a future 1t-style mode
would move the depth). BS1/BS2 keep the core default (1) untouched - proof in the
f241d54 commit. Hypothesis notes below kept for the derivation trail.

### The "jumpy camera" hypothesis and the pose-lag A/B (s43b, verdict pending)

User percept (s42, refined s43b): head-coupled camera motion is jumpy/bouncy, "as if
every micro movement is translated and the game compensates with more or delayed
movement" - not present in BS1/BS2. The named candidate: POSE-ATTRIBUTION MISMATCH
(reprojection wobble). Core's `g_viewsContent` submits captured eyes with the locate
generation ONE back, on the lockstep assumption "locate N feeds the tick that
presents at N+1" - calibrated on BS1's 1T renderer (the M4 head-bobbing fix). But
Infinite's substrate is THREADED and ring-buffered with `OneFrameThreadLag=True`
(DR-I5), so its presented content may lag the locate by TWO generations; a
one-generation mis-attribution is a constant one-period pose error that scales with
head speed - the compositor over/under-reprojects every frame, which is exactly the
described feel. The wrong-direction alternative (content actually FRESH, lag 0) is
also covered.

The instrument/fix is one selector: `bvr::vr::set_pose_lag(0|1|2)` (core, default 1
= the historical behavior, BS1/BS2 never call it - byte-identical), applied at the
SR capture's eye-pose attribution only. Seams: `bsipose 0|1|2` (desktop) and the F10
"POSE ATTRIBUTION (jumpy-camera A/B)" radios under VR stereo, with the
inter-generation head delta (deg/pair) readout = the error magnitude at the current
head speed. In-headset discrimination: turn the head steadily at each setting; the
lag that kills the wobble NAMES the pipeline depth. (If none of the three is clean,
the residual is drive-side - pose age at the game-thread consume, or engine camera
smoothing on top of the drive - and that is the next instrument, not a blind fix.)

### The ini propagation lane, PROVEN (s43)

`XGame\Config\DefaultEngine.ini` (game folder) IS the source the boot-derived
per-user XEngine.ini regenerates from: changing
`TimeBetweenPurgingPendingKillObjects` 30 -> 300 in DefaultEngine.ini:300 appeared
verbatim in XEngine.ini:98 on the next boot. This is the write lane for every
[TextureStreaming]/[Engine.Engine]/[SystemSettings] lever (XEngine.ini itself is
never edited - hard rule upheld). Discipline: timestamped `.bvr-bak-s43` backup
beside the original before the first edit; probes reverted after reading.

## LIVE RESULTS (session 42 - I6 judder flat half; I7 opens: the exec-surface truth, object dispatch, the pad lane)

### The judder, measured flat: the wait GATES here, and the instrument now rides every session

The s41 suspect was "77-80 pairs/s free-running against VD's 72 Hz = a ~5 Hz beat". New core
instrumentation (TRACE pairs line in pacetrace.log, 1 Hz: inter-pair interval mean/sd/min/max,
plus **waitGate** = present-thread ms/s actually blocked in the xrWaitFrame handoff, plus the
runtime's own `predictedDisplayPeriod` - never consumed anywhere before) says the SIM cannot
reproduce a free-run:

| sim refresh | pairs/s | interval mean | sd | min-max | waitGate |
|---|---|---|---|---|---|
| 72 | 72 | 13.89 ms (= period) | ~1.2 ms | 10.4-17.4 ms | 540-620 ms/s |
| 90 | 90 | 11.11 ms (= period) | ~1.0-1.2 ms | 7.8-14.5 ms | 534-548 ms/s |

The sim's `xrWaitFrame` strictly blocks, so the pair rate LOCKS to refresh with the present
thread parked in the handoff. A runtime that PIPELINES (returns waits early) is the one that
free-runs - whether VDXR does is exactly what the TRACE pairs line will show from the user's
next headset run (read `%LOCALAPPDATA%\BioshockVR\bsi\pacetrace.log` afterwards: pairs/s vs
72 and waitGate ~0 = free-run confirmed; pairs ~72 with fat sd/max = marginal-frame-time
flutter instead, and the resolution picker is the lever).

**THE HEADSET RUN ANSWERED IT (user, VDXR via Virtual Desktop, 2026-08-06 00:09-00:41).**
The trace from the run (pacetrace.log, kept): VDXR reported **period 12.50 ms = 80 Hz**
(the VD session ran at 80, not 72). In steady seconds the pacing is LOCKED AND CLEAN -
pairs 80/s == refresh, interval mean 12.5 ms, sd 0.3-1.0 ms, sync gating on our side.
**The s41 free-run-beat hypothesis is FALSIFIED on VDXR too.** The judder is something
else entirely: recurring HITCH SPIKES - single pair intervals of 39/43/51/75/77/87/101/113
ms (3-9 missed display frames each) in bursts every few seconds, visible as sd exploding
to 3.6-10.7 ms in exactly the bad seconds. User percept matches the numbers: judder worse
OUTDOORS at native 2064x2208 (heavier scenes -> more/longer spikes, and render cost near
the 12.5 ms budget drops extra frames), noticeably better at the `eye` preset 1600x1712.
Candidate spike sources for the hunt (next session): UE3 texture-mip streaming on view
change (would bind hitches to head TURNS - the reported trigger), UE3 incremental GC,
shader-cache misses, the VD encoder. Second user observation, recorded open: camera
movement feels "a bit jumpy" beyond the hitches - candidates: the hitch bursts at micro
scale, the one-pair-stale content-pose attribution, or drive granularity; instrument
before theorizing.

The armable fix either way: **`vrpace sync on|off|<hz>` / `set_pace_sync`** (core, default
OFF, the set_pace_detach pattern; Infinite arms it inside `apply_vrstereo(true)` and disarms
on the symmetric off). Before OPENING a pair the present thread waits for the next tick of a
one-period-per-pair schedule (period = predictedDisplayPeriod, else the commanded Hz); the
closing RIGHT present is never delayed (the 1-4 ms intra-pair gap survives); the schedule
self-collapses to no-delay when the runtime is slower (measured: sync-on at refresh 72 keeps
72 pairs/s, tightens sd 1.2 -> 1.0 ms, moves the gate to our side - waitGate 615 -> 21 ms/s -
and the SR beat stays exact 72/72/144/72). F10 checkbox "Sync pair rate to headset refresh"
under SR pair pacing is the in-headset A/B. BS1 inertness proof run on the build: full sim
lane, claimRatioH 1.01769 == the banked 1.018, no sync log line, zero faults.

### The exec surface, corrected: SCRIPT execs are dead through ConsoleCommand; only C++ handlers live

s37 proved `bsiexec setres/shot` by effect - those are C++ `Exec` handlers. Session 42
extends the map with measured negatives IN A GAMEPLAY SAVE: `god`, `AllWeapons`,
`behindview`, `viewmode wireframe` all dispatch through ConsoleCommand and produce ZERO
effect (screenshots pixel-identical; wireframe would have rewritten every pixel). The
script-side console exec bridge is compiled out or gated in retail, exactly like the key-bind
lane (s34). Corollaries, all verified live:

- The give-family console cheats DO NOT EXIST in this build: no GiveWeapon / GiveVigor /
  GiveItem / GiveLockpicks / giveall FName anywhere in a full in-save GNames dump (69,719
  entries). GIVELOCKPICKS in the autocomplete ini is a stale string, not a command.
- `XCheatManager` is NEVER INSTANTIATED: the PC's field walk shows CheatClass (a `Class`
  pointer at PC+0x344, right after XPlayerInput +0x340) and no manager instance anywhere in
  the PC's fields. `EnableCheats` is not on the PC's function chain (FindFunction null). So
  God/Ghost/Walk/Slomo/Loaded/AllWeapons - all CheatManager script vocabulary - are
  structurally unreachable, not merely gated.
- **The lane that works: ProcessEvent on the OWNING object** (the s34 prediction, now the
  design). `bsicallat <hexaddr> <Func> [float]` dispatches on any object a `bsifields` walk
  named; the vtable slot interlock (+0x54/+0x7C RVA match) carries over unchanged.

### The working command list (proven live this session; effects state-confounded where noted)

| command | object | result |
|---|---|---|
| `bsiexec LoadCheckpoint` | console (C++/save system) | **LOADS THE NEWEST CHECKPOINT FROM THE MENU** - the autonomous save-entry lane the harness lacked. Proven twice by GNames growth (62,160 -> 69,719), camera relocation and the gameplay HUD. One caveat: it picks the engine's own "most recent" save. |
| `bsicall NextWeapon` / `SwitchWeapon` | PC (UFunction) | dispatches cleanly; on the pistol save NextWeapon cycles onto the single owned gun (idle-sway-only diff) - a second weapon is needed for a visible swap |
| `bsicall NextPlasmid` | PC (UFunction) | **PROVEN BY EFFECT on the 2-vigor save** (2026-08-06): vigor icon swapped with the transition spark, left hand raised to cast, crosshair appeared - screenshot diff 4.6 mean / 8.5% pixels |
| `bsicallat <pawn> AddInvulnerableFlag` | XHuman | dispatches (god-family native; flag arg semantics underived - zeroed parm; re-confirmed on the pistol save's pawn - damage-effect verification pends a combat test) |
| `bsicallat <pawn> AddDefaultInventory` / `SetWeapon` / `CreateInventory` | XHuman | dispatch cleanly with zeroed parms; NO weapon appears - Infinite grants via story Kismet, and both user saves predate the first weapon |
| `bsicallat <pawn> AcquireWeapon` | XHuman | EXISTS and FAULTS on null parms (SEH-swallowed) - **it wants a weapon object argument; this is the s43 grant seam** |
| `bsinames dump` | - | full GNames -> `%LOCALAPPDATA%\BioshockVR\bsi\gnames.txt` (game-derived, never commit). ~1.1 s for 69.7k names |
| `bsifields [startHex] [dwords]` | latched PC | UObject field map with class names (see below) |

Recorded negatives (do not re-probe): GiveAmmo/GiveMoney/RefillWeaponAmmo/RefillSalt/
AllWeapons/Loaded/God/Ghost/AddItemToLoadout/EquipPlasmid are on NEITHER the PC nor the
XHuman function chains (FindFunction null on both) - they are Kismet/designer-action names
or CheatManager vocabulary. `Fly`, `Summon`, `KillAllPawns`, `SetJumpZ`, `SetSpeed` exist as
names; owning objects unknown.

**The s43 loadout options, in preference order**: (1) the user plays to the raffle once and
saves - a post-pistol save makes grants unnecessary for aim work *(DONE 2026-08-06: the
TWN2 save carries pistol + 2 vigors)*; (2) derive the weapon archetype lookup
(DynamicLoadObject/StaticFindObject by path) and feed `AcquireWeapon` a real object via an
object-arg extension of bsicallat; (3) walk XHuman's fields (bsifields needs a small
explicit-object generalization) for the loadout/inventory manager.

### The GRANT recipe (s42b, 2026-08-06 - every piece proven, final combination pending)

Lane (2) above is BUILT and all but the last step is proven live:

- **`bsiload <Full.Object.Path>`** = DynamicLoadObject(path, null, MayFail=1) on the PC,
  logging the returned pointer + class + name. Stock parms shape works on this build.
  TRAP FOUND AND FIXED: command.txt args carry the trailing newline; ConsoleCommand's
  parser eats it but an object-path match is exact - trim before converting.
- **The script-class package is `XCore`** - `XCore.XConsole` AND `XCore.XWeaponUndertow`
  both resolved (the latter = a Vigor class the save does NOT own, pointer in hand);
  `XGame.*` and `Nano_*.*` are recorded negatives for class paths (Nano_* are startup
  package names, not the class outers).
- **`bsicallat <obj> <Func> 0x<ptr>`** passes a raw pointer at parms+0 (the
  class<Inventory> shape CreateInventory wants); CreateInventory dispatches on XHuman.
- The one unexecuted step: `bsicallat <pawn> CreateInventory 0x<vigorClass>` in a
  possessed gameplay state, then NextPlasmid onto it. Blocked at 01:2x only by the
  LoadCheckpoint state machine (below), not by any missing machinery.

### The GRANT combination, EXECUTED and FALSIFIED (s43, 2026-08-06)

The full combination ran on the TWN2 save (pistol + 2 vigors) and DID NOT GRANT - the
dispatch machinery is fine; the registration step is what's missing:

- `bsiload XCore.XWeaponUndertow` -> class 14299130; `bsiload
  XCore.Default__XWeaponUndertow` -> the CDO 14299288 (class XWeaponUndertow). Both
  resolve; the `Default__<Class>` CDO path shape works.
- `bsicallat <pawn> CreateInventory 0x<class>` dispatches and RETURNS - but the
  NextPlasmid cycle stays 2-long (Devil's Kiss <-> the save's second vigor, cycled 4x
  by screenshot). The created Inventory (if any) never registers in the plasmid list.
- `bsicallat <pawn> AcquireWeapon 0x<CDO>` dispatches and RETURNS with a real object
  parm (vs the SEH fault on null parms - progress on the parm shape), but neither the
  NextPlasmid nor the NextWeapon cycle grows (vigors here ARE XWeapon* subclasses, so
  both cycles were checked; the weapon cycle still holds only the pistol).
- READ: equipping in this engine goes through an index/list layer (GNames has
  ClientSetEquippedPlasmidIndex / EquippedPlasmidIndex / BackupPlasmidIndex /
  EquipPlasmid - the last a recorded FindFunction negative on PC and XHuman), so the
  grant seam is on the LOADOUT/INVENTORY MANAGER object, not the pawn's top-level
  functions. Next rung when this lane resumes: generalize `bsifields` to walk an
  explicit object (the s42 option-3 note), walk XHuman for the inventory/loadout
  manager, and dispatch the list-registration function there. NOT pursued further in
  s43 - the stutter hunt owns the session (10-min timebox honored).

### LoadCheckpoint's state machine (s42b, measured the hard way)

- From a SETTLED main menu (save index loaded): loads the NEWEST disk save. Proven twice
  (00:59 boot -> TWN2 pistol save, GNames 72,073).
- Dispatched too early (title screen / index still loading): starts the CAMPAIGN INTRO
  instead - and once the intro has run, the in-memory "current checkpoint" is
  contaminated: every further LoadCheckpoint in that process re-loads the intro, and
  `disconnect` + LoadCheckpoint goes black/menu-limbo. A fresh process is the only clean
  reset (the intro run writes NO save file - the user's saves are untouched).
- The reliable sequence: fresh boot -> DISMISS "PRESS ANY KEY" (needs a real keypress -
  game-click alone did not advance it; `game-key.ps1 -Game bsi -Key Space` since s43) ->
  wait for the menu ~30 s -> `vrcmd` confirms pump=game -> ONE LoadCheckpoint -> 45 s ->
  verify by GNames/pawn walk, THEN probe.
- s43 adds a FOURTH observed behaviour: the load can be SLOW - a LoadCheckpoint
  dispatched at a settled menu (pump=game confirmed) can take 60-100 s before the
  transition fires (boot 3: dispatch ~15:41:1x, pawn still absent at +35 s, letterbox
  transition at +60 s, pawn present after). It first read as a "silent no-op cured by
  a retry" (boot 2: retry + load completed together) - the truer model is one slow
  load. Discipline: after dispatch, poll `bsifields 1F0 8` every ~30 s up to ~2 min
  before concluding anything; a retry into a load-in-flight has not misfired yet but
  is unnecessary. The
  intro-contamination hazard is only when the dispatch lands at title/attract; the
  no-op case is safe to retry.
- The s37 pre-existing freeze hit AGAIN at boot 1 of s43 (frozen at the attract movie
  ~40 s in, before any seam command: Responding=False, sim frames frozen at 902, log
  silent from the letterbox-off line). Force-kill + relaunch worked first try -
  protocol unchanged.

### The PC field map and the new struct facts (derived live, s42)

`bsifields` on the latched XPlayerController (TWN save): pawn `XHuman` at **+0x1FC** (also
+0x268/+0x274 - ViewTarget shapes), `XPlayerReplicationInfo` +0x200, `XLocalPlayer` +0x23C,
`XCamera` **+0x240**, HUD +0x2B4, `XPlayerInput` **+0x340/+0x348**, CheatClass (Class)
+0x344, `XnaForceFeedbackManager` +0x35C, `OnlineSubsystemSteamworks` +0x390, XWorldInfo
+0xA4. **UObject::Name is at +0x18** (index dword; derived by candidate walk on the PC's
class object reading 'XPlayerController'; Class stays +0x20). **FName indices are per-boot**:
the same name pool loads in package order, so indices from a previous boot's gnames.txt are
stale - re-dump per boot; only the text is stable.

### The pad lane (I7 controls): IAT verified live, end-to-end effect flat

`bsiinput on` verified the s34-derived ord-2 IAT slot (patterns.h `kXInputGetStateIatRva
0xCD4814`): current target resolved into XINPUT1_3.dll (the proxy chain), then re-pointed it
at core's composing wrapper (`hijack_import_slot`) and enabled the bridge. Measured in a
gameplay save: the game polls through the wrapper (iat 5642 calls at first status read; the
real chain answers ERROR_DEVICE_NOT_CONNECTED 1167 and the bridge composes CONNECTED),
**sim right-stick hold TURNS the camera** (engine yaw 65.1 -> 145.3 -> -133.1 deg across two
holds in the heartbeat; screenshot diff 31.4 mean-abs / 52% pixels), sim A press produced a
one-sample camera kick (pitch 36 / roll -20 units). Left-stick translation did NOT move the
pawn in this scene - the checkpoint drops into a movement-locked scripted state (loc frozen
across every probe); re-test walking in a free-roam scene before reading it as a defect.
NO UpdateInput pump and no SetUseController on this engine - it polls XInput itself, so
BS2's whole activation machinery correctly does not port.

### Session hazards (s42)

- The pre-existing freeze hit ONCE at a LoadCheckpoint dispatched into the attract-movie
  window (Responding=False, presents 0, silent log; watchdog stack captured in pacetrace -
  ntdll wait, zero mod frames). Force-kill + relaunch; the second attempt (dispatch AFTER
  `vrcmd` confirmed pump=game at the settled menu) loaded clean twice.
- gnames.txt is game-derived content in the data dir - never commit (hard rule).

## LIVE RESULTS (session 41 - I6: the FOV lever, the lens decoder, resolution, config)

### The FOV lever: every named lane is dead, and the live chain is a per-tick recompute

The named-property route was tried FIRST, per the roadmap, and is now a recorded negative
with the mechanism mapped:

- `bsiexec set XUserOptionsManager FieldOfView <v>` dispatches cleanly (the class and both
  property names are in GNames: 11162 / 5085 / 5046) but WRITES NOTHING - after
  `set ... FieldOfView 0.37`, a full writable-memory scan for 0.37f finds ZERO stable
  holders. Same for in-range 0.0 and absurd 3.0 by frustum decode. Do not re-try `set` on
  this class. (`set D3DRenderDevice ...` is bound to F7/F8 by the retail game, so the exec
  itself is not dead - this class just is not writable through it.)
- Console `FOV 100` and `SetFOV` execs: no frustum effect (`SetFOV` is in GNames at 20771
  but FindFunction on the PC chain returns null; the CheatManager route was not pursued).
- The LIVE chain, mapped by poke/rescan at the attract: with the slider at max the frustum
  decodes tanH 0.8770 and the camera object `[pc+0x240]` carries **82.50f** - and
  tan(82.5/2) = 0.8770 EXACTLY. Copies at `[cam+0x214]` (followed by two 1.3333f - the
  ACamera DefaultFOV / DefaultAspectRatio shape) and `[cam+0x3D0]` (cached POV: loc +0x3B8,
  rot +0x3C4, fov +0x3D0 - FTPOV). A memscan for 82.5f found SIX holders and EVERY ONE
  snapped back within a tick of being poked: the value is recomputed from the option each
  tick; no address is the source.

**THE LEVER (patterns.h `kCameraDefaultFovOffset`/`kCameraPovFovOffset`): enforce per
camera-detour dispatch.** `apply_fov_lever` writes both copies every GetPlayerViewPoint
call, outrunning the once-per-tick refresh; disarm self-restores because the engine's own
recompute is the undo. Measured at 2560x1440: commanded 110 -> decode tanH 1.4281
(= tan 55 exact) tanV 0.8033; 130 -> 2.1443/1.2063; monotone, aspect held, 56k writes
0 faults; `bsifov off` -> the native value returns within a tick. SR beat with the lever
armed stayed exact (84/84/169/84).

### THE LAW'S ANCHOR, corrected by the decoder at 1:1 (the biggest s41 finding)

The camera degrees value is horizontal **at a FIXED 16:9 REFERENCE**, not at the current
aspect: `tanV = tan(deg/2) / (16/9)` is pinned, `tanH = tanV x actual aspect` (the s37
vertical-referenced law restated WITH its anchor). Measured at 1440x1440 with the lever at
100: both decoders read tanH = tanV = **0.6704 = tan(50)/1.7778 exactly**, while the claim
(then computed against the current aspect) sat 43.7% off - **the lens decoder's audit
caught the wrong law on its first non-16:9 round**. At 16:9 the two readings coincide,
which is why no 16:9 measurement could separate them. patterns.h `kFovRefAspect` carries
the constant; the corrected claim closes at delta 0.0% and the capture read claimRatioH
0.48705 vs 0.4871 predicted.

Consequences, measured: claimRatioH is config-dependent (slider-min/16:9 baseline 0.5576;
lever-100/16:9 0.86586; lever-100/1:1 0.48705 - the square render ALONE narrows the
horizontal, the ROADMAP caveat now with numbers). Filling the ~110-deg eye vertically
needs lever ~137 deg (tanV 1.428); the banked `eye` preset carries exactly that plus the
1600x1712 render.

### The live lens decoder (bsilens) and its vote

Core half: an opt-in UpdateSubresource tap in frame_inspector (raw 80-byte samples into a
seqlocked ring; disarmed cost one relaxed load; only this adapter arms it). Adapter half
(`lens.cpp`): 1 s rounds on the game thread; per sample the matrix decode (gates aligned
with decode-framedump's Decode-Matrix), then the load-bearing structural filter
(tanH/tanV == live backbuffer aspect within 1%), clustering by tanV, publish only >= 60%
of >= 16 valid samples, runner-up >= 10% published as a NAMED second lens (telemetry
only), refused rounds keep the last lens with age showing. Measured: caught the stale
slider-min claim on its first round (decoded 0.4933 vs claim 0.4317, 14.3% loud); follows
the lever within one round at 100% support; `bsilens track on` writes the claim on
majority rounds (the armed lever still wins - publish order). No second lens observed at
the attract (no viewmodel there); the gameplay-save check rides the headset session.

### Resolution: both lanes proven end-to-end, and the boot acceptance

`bsires squareperf` resized the backbuffer live to 1440x1440 (ResizeBuffers hr=0, XR
swapchain rebuild survived) AND wrote XUserOptions.ini `[XCore.XUserOptionsManager]`
ResolutionX/Y (section-scoped, one-time .bvr-bak-res backup, temp+ReplaceFileW, read-back
logged as not-acceptance); the NEXT BOOT's first Present read **backbuffer 1440x1440** -
the DR-I8 acceptance on the mod's own write path. XEngine.ini is never touched. The live
user config's single section `[XCore.XUserOptionsManager]` carries every key this
milestone needs (FieldOfView :138, MaxUserFOVOffsetPercent :95, ResolutionX/Y :150-151).

### Config registry and presets

Adapter-local `config.cpp` registry (six keys) serves vrpreset.ini (legacy files load
unchanged), named presets under `bsi\presets\`, the verb family and the F10 slots.
Round-tripped within a session and across a restart (6 applied each way). A loaded
preset's resolution is LATCHED into the picker, never auto-applied. Core extraction
deferred (ARCHITECTURE decision log 2026-08-05).

### Session hazards recorded

The pre-existing unattended-attract freeze hit TWICE (force-kill + relaunch, zero mod
faults - same signature as s37's, which hit an unmodified build). And at attract movie
transitions the game-thread pump can lag 30+ s: send seam commands one at a time and
CONFIRM the dispatch line before the next, or later writes overwrite undispatched ones
(VERIFICATION gotcha 20).

## LIVE RESULTS (session 40 - I5: stereo, and the render-root derivation)

### The FOV claim and Infinite's own claimRatioH baseline

The adapter publishes `hfov = 2*atan(tanV x aspect)` per detour call (the law is
vertical-referenced, session 37) with `publish_gameplay_view(true)` alongside - core's
cinematic fallback defaults ON and a stale publish parks the headset on the quad, so the
publish must tick every dispatch. tanV default = `kTanVSliderMin` 0.4317 (the shipped slider
minimum); there is NO live option reader until I6, so a moved in-game FOV slider makes the
claim stale - `bsifov tanv` corrects it and `dumpframe cb` verifies it. Measured on the sim
at 2560x1440: audit `tanH=0.767467 src=readback` (= 0.4317 x 16/9 exact), and with the sim
eye symmetric at 54 deg (`fov 54 55 54`) the capture reads **claimRatioH 0.5576** =
0.76747 / tan(54 deg) to four digits. That is Infinite's OWN baseline at slider-min/16:9 -
NEVER compare to BS1's 1.018 (different game, different law, different meaning).

### The render-root chain, derived live (the session's core work)

The scene camera, scene build and present kick live at three levels, all now named:

| level | RVA | role |
|---|---|---|
| viewport draw (THE DOUBLING ROOT) | `0x1FDE30` | FViewport::Draw analog: stack canvas ctor `0x331110` -> client Draw dispatch -> canvas dtor `0x3339F0` -> present kick `call 0x1E50B0(1)`. thiscall on the viewport, ONE stack arg, `ret 4` (at `0x1FE0AB`). 4 static E8 callers; the per-tick gameplay dispatcher returns to **`0x206309`** |
| client draw | `0x26A3E0` | UGameViewportClient::Draw analog, reached via vtable `.rdata 0xDE6FC8` slot 2 (+0x8) -> jmp stub `0x6F1360`. Body holds the IsA-gated (UClass interval `+0xC0/+0xC2`) player-controller loop that calls GetPlayerViewPoint once per present per controller (call site ret `0x26B499`); the client object is `[viewport+0x1C]`, dispatch site ret `0x1FE05F` |
| camera seam | `0x1E10C0` | GetPlayerViewPoint (I2), unchanged |

**Derivation method (all live, one relaunch each, then static confirmation):** (1) a caller
census at the camera detour - ret `0x26B499` fired EXACTLY once per present (810 in 810)
on the game thread; (2) `bsicam stack` - one-shot RtlCaptureStackBackTrace PLUS a raw stack
scrape for call-preceded image addresses (FPO cuts the API short at 4 frames; the scrape
walks the whole chain); (3) `bsicam scenedraw` / `bsicam vtprobe` - one-shot SEH-guarded
reads that resolve virtual dispatch targets from live objects. The outer entry fell out of
the scrape: ret `0x206309` is an E8 whose target is `0x1FDE30` directly, and the capstone
stream from `0x1FDE30` reaches the dispatch at `0x1FE05D`. Static walking alone had FAILED
twice first (function-start heuristics land on basic-block boundaries; .rdata pointer scans
drown in UTF-16 false positives) - on this engine, derive render roots live.

**The negative that chose the root (recorded so nobody re-tries it):** doubling the CLIENT
draw (`0x26A3E0`) doubles the camera and scene build but NOT the present - the present is
kicked by the viewport draw's tail, outside the client call tree. Measured: the SR tag ring
skewed +1 per tick (self-healing at depth 3, continuous "sr tag ring skewed" log spam) with
presents pinned 1:1 to ticks. Doubling the VIEWPORT draw gives camera + scene + present per
eye: presents/s snapped to exactly 2x draws/s.

### SequentialReentry: THREADED doubling works on this substrate (DR-I5 confirmed by use)

No 1t machinery, no flush-point hook, nothing from BS1's kit - the doubled call runs on the
threaded ring-buffered substrate exactly as DR-I5 predicted. Measured at the attract (which
runs real gameplay scenes), sim at 90 Hz:

- `draws/s=90 2nd/s=90 presents/s=180 camReplays/s=90` - every gate exact at the sim's
  ceiling (90 eye pairs/s), second call costs **80-170 us**.
- Pass 2 replays pass 1's CACHED base absolutely (100 ms staleness guard) and applies the
  +1 eye; pass 1 caches pre-eye and applies -1. Inter-eye |d| = ipd x scale exact (3.150 UU
  at 63 mm / scale 50; doubles at scale 100). The replay-burst counter (one per doubled
  draw, seq-edge) equals the second-draw count exactly - the per-frame-multiple of this
  seam (several camera dispatches per draw) makes the RAW pass-2 call count a multiple, so
  the burst counter is the gate.
- **L/R parallax proven at the pixel level**: SR capture pair differs (mean-abs-diff 0.42,
  1.09 % channels changed) while the rung-1 mono-projection pair is byte-identical (0.0) -
  the control that makes the diff meaningful.
- The deny gate fired in anger during the battery: foreign-caller skips counted 2 (a
  non-gameplay dispatcher reached the root at a menu transition and was refused, no double).
- Occasional `sr tag ring skewed - cleared` resyncs at attract scene/movie transitions
  (self-healing by design, mono for 2 frames); zero watchdog fires, zero faults in the soak.

### Sim traps found (session 40)

- PowerShell 5.1 turns cmake's stderr *deprecation warning* into a NativeCommandError when
  the build script re-runs the configure step (CMakeLists edit) - the build actually
  succeeded. Wrap the build in `powershell -NoProfile -Command ... 2>&1` from bash, or
  ignore exit 1 when the tail says "Installed".
- game-shot's `-Out` is used verbatim (no .png appended): pass extensionless names to
  img-diff or name the file with the extension yourself.

## LIVE RESULTS (session 39 - I4: the 6DoF drive, proven flat with numbers)

The I4 drive is live and the whole flat battery passed on the simulator at the attract
(details and the exact numbers: TESTING.md "I4 battery"). Everything below is a design fact
or a measured result worth keeping; nothing here is a new address - I4 consumed only
already-derived offsets.

### The drive design (what I5 builds on)

- **Write target: the detour's out-params ONLY.** `drive_view` runs in the GetPlayerViewPoint
  detour tail, after the original fills the out-params and after the read-only snapshot, and
  substitutes them. `[cam+0x3B8]`/engine memory is never written, so drive-off is a
  byte-identical passthrough, and the engine's own view keeps moving under mouse/pad -
  observed live: engineRot swept with the attract camera on every beat while the drive held
  the final rot. The BS1 pitch-freeze class of bug cannot occur by construction; the
  read-back discipline is the heartbeat printing engineRot (pre-drive snapshot) next to the
  final rot, plus pitchErr. pitchErr is LOGGED ONLY - publish_vr_gameplay/publish_pitch_error
  would arm core's shared pitch kill and seize right-stick Y (xinput_bridge), so the servo
  lane is deferred to I7 with the rest of input.
- **Rotation law**: yaw ADDITIVE (game yaw + wrap_rot(headYaw - recenterYaw), integer rotator
  units), pitch/roll ABSOLUTE from the head. Position: recenter-relative XR delta ->
  `xr_to_ue` -> rotate into recenter-local by -recenterYaw, out by the game yaw -> x
  worldScale -> ADD to the game loc; Z is world-up, unrotated. All in
  `src/game/bioshockinf/inf_math.h` + `camera.cpp` (adapter-local copies per the decoupling
  directive).
- **The UE3 frame MATCHES the Vengeance convention** - now measured, not assumed: each axis
  was driven separately flat (simhead yaw/pitch/roll sweeps, the sim's real `head rot`/`head
  pos`/`head orbit` path) and every predicted rotator unit and UU landed exactly (30 deg =
  5461 units additive; 20 deg pitch = 3640; 10 deg roll = 1820; 0.5 m = 25 UU at scale 50,
  rotated by the game yaw for horizontal axes, world-up for Z).
- **MonoTracked runs UNDER the quad, camera mode never set**: core's `set_camera_mode(true)`
  flips submission from the quad to a projection layer (`openxr_runtime.cpp` projectionMode)
  - that is I5's rung. The I4 live lane gates adapter-locally on `bsicam drive` +
  `get_head_pose` succeeding; `vr_camera_mode()` is deliberately NOT consulted. Core's
  "never let a head-driven camera show on the quad" comment is BS1/BS2 convention that this
  ladder intentionally breaks for one rung (ARCHITECTURE decision log 2026-08-05).
- **Lane order** (BS1's proven shape): vrrec replay -> simhead (deadline) -> live -> off.
  While a replay is loaded the live/sim lanes are never consulted. simhead carries BS2's
  position triple; arming from idle requests a recenter onto the first sim pose.
- **worldScale default is 50 UU/m** - UE3's canonical 1 uu = 2 cm, NOT BS1/BS2's calibrated
  100 (different engine; never copy a number). The flat battery only proves the code applies
  the configured scale; the true value is the user's headset calibration (F10 slider,
  `vrpreset save` persists to `bsi\vrpreset.ini`).
- **vrrec cadence is a present_count() edge**, not per-detour-call: this seam fires
  1000-9600/s (many times per rendered frame), so BS1's once-per-CalcView tap does not
  transfer. Both record and replay advance on the same edge - measured ~90 entries/s live,
  and a sessionless replay ran the cursor at the uncapped flat present rate (faster
  wall-clock, frame-for-frame identical: determinism over wall-clock, as designed).

### Measured on the merged path (worth keeping)

- **Record/replay round trip is exact**: 1077 frames recorded across a `head orbit 10 8000`,
  `[rec] PLAY` marks number-for-number identical to `REC` (head quat AND driven camera),
  recenter yaw + worldScale restored from the BVRR header, `lane=replay` with `xr=none`.
- **Rendered-pixels acceptance** (the engine consuming the substituted view): window
  img-diff floor 0.58 mean / 1.12 % vs simhead-yaw-60 6.12 mean / 23.4 % - 10x, with the
  heading visibly rotated. The compositor capture was NOT usable for this - see
  VERIFICATION gotcha 17 (sim recenter-while-yawed capture trap, found this session).
- **90.0 fps sustained with the drive on** (3915 frames / 43.5 s between two sim status
  reads), live lane calling `get_head_pose` (a mutex) on every detour call at 1400-3600/s -
  no measurable cost at this game's dispatch rates.
- The attract sequence runs REAL gameplay scenes (health bar, path-2 camera, moving loc) -
  the flat battery ran there attended; per-beat checks (final-minus-engine) are
  scene-independent, which is what made that valid.

## LIVE RESULTS (session 38 - I3 sim battery: the whole XR stack runs on Infinite)

The merged BS2 OpenXR runtime ran the full mono-big-screen stack on `BioShockInfinite.exe`
against the simulated Quest 3, unmodified: **zero core changes and zero adapter changes were
needed for bring-up** (the only new mod code is `bsivr on|off|status`, an adapter-local wrapper
over `vr::set_enabled` for scripted A/B). Every number below is from a live run.

### The bring-up, measured

- Instance -> session on the game's OWN device, first try: `first Present: backbuffer 2560x1440
  format 28` (R8G8B8A8_UNORM) -> `swapchain pair 2560x1440 format 29` (R8G8B8A8_UNORM_SRGB -
  core's picker prefers SRGB; same typeless family, so the zero-copy `CopyResource` is legal) ->
  IDLE -> READY -> SYNCHRONIZED -> first quad frame -> VISIBLE -> FOCUSED, all inside ~600 ms of
  the first Present. Pace thread up (`xrWaitFrame` off the present thread).
- **90.0 fps sustained** in gameplay (sim at 90 Hz); `step 5` grants exactly 5 frames; the game
  window keeps presenting at full rate through every unfocus/idle episode (flat rendering
  continues by design).
- `focus lose` -> VISIBLE: frame rate holds (session-33 oracle), the mod KEEPS submitting
  (session-28 requirement observed live: begin/end continue in VISIBLE), FOCUSED re-earned.
- Teardown/re-bring-up, three independent lanes, all clean: `bsivr off/on` (teardown ->
  full re-bring-up to FOCUSED in ~250 ms), `hazard waitfail 1` (SESSION_LOST -> teardown ->
  fresh session after the 5 s cooldown), and the resize lane below.
- **The resize lane** (Infinite-specific, DR-I8's lever exercised against XR):
  `bsiexec setres 1600x1200` live -> `ResizeBuffers 1600x1200 hr=0` -> queued XR swapchain
  rebuild at the safe point -> `swapchain pair 1600x1200`, quad size follows aspect
  (2.4 x 1.8 m at 4:3), hfov recomputed 137.0 -> 124.6 deg, capture still carries game pixels.
  And back to 2560x1440 the same way. Mid-session backbuffer resize is fully survivable.
- Quad geometry verified in captures: world-locked LOCAL quad, 2.4 m wide at 1.75 m, correct
  stereo parallax between eyes, game pixels (health bar, scene, menus) visible and readable.

### Three SIM bugs found and fixed (the battery's real yield - first mono-quad consumer)

1. **`now_xr_time` int64 overflow wedged the pace thread permanently.** The conversion
   `(QPC_ticks * 1e9) / freq` overflows after ~15 min of MACHINE UPTIME at a 10 MHz QPC, making
   the sim's clock a ~30.7-min sawtooth that jumps backwards ~1845 s. A session crossing the
   jump computes a huge free-mode `waitNs` and parks `xrWaitFrame` for good; the mod then
   (correctly, per the 1:1 wait:begin spec) never waits again - observed live 25 min into
   gameplay as `SUBMISSION IDLE (frame not begun)` forever, `waitFrames = beginFrames + 1`,
   every present timing out its 200 ms handoff deadline. **Derivation: minidump of the wedged
   process; the pace thread's stack ends in `impl_WaitFrame`'s `wait_for` (xrsim_frame.cpp:231);
   capture JSONs show sawtooth `displayTimeNs` (278 s / 824 s, not days-magnitude).** Fixed with
   split seconds/remainder arithmetic PLUS a defensive clamp: any computed free-mode wait > 1 s
   logs and resyncs instead of blocking - no clock anomaly may hang the host again.
2. **Quad captures were sRGB-decoded to linear, crushing dark scenes ~13x.** The app swapchain
   is `_SRGB`, the compositor SRV decodes to linear, and the old UNORM composite target stored
   those linear values: an Infinite gameplay capture read meanLuma 0.05 / nonBlack 0.03 % while
   the window read ~10 (i.e. numerically "black" with visible content). Composite target is now
   `_SRGB` so texel bytes round-trip. **Pixel-stat baselines recorded before session 38 are
   systematically darker and not comparable** - BS1 re-baselined below.
3. **Timed `focus lose <ms>` was silently sticky**: `session_focus_lose` stored the hold, then
   `session_force_state`'s FOCUSED->VISIBLE edge reset it to sticky - only an explicit
   `focus regain` ever recovered. Unnoticed because BS1's sequence uses the explicit form.
   Write order fixed.

### Sim traps for the next session (harness discipline, not bugs)

- **The sim's initial LOCAL origin is at the FLOOR (world origin), not the initial eye pose** -
  OpenXR (and VDXR) put LOCAL at the initial VIEW pose. The mod's eye-level quad therefore
  renders 1.6 m low / grazing until you send **`recenter`** (which re-origins LOCAL onto the
  head, spec-shaped). Send `recenter` before any quad-pose capture. Left unfixed in the sim this
  session (shared-tool blast radius); candidate for the healing lane.
- `idle on <ms>` is PERSISTENT (every wait blocks <ms> until `idle off`), not a one-shot window.
- After `step N` exhausts its credits the next `pace free`/`step off` cannot commit until the
  30 s starve grant (commands apply at wait boundaries) - send `pace free` while credits remain,
  or wait it out.
- Infinite auto-pauses on window-focus loss (pause menu up, presents continue, XR submission
  idles on the "frame not begun" path) and auto-resumes on focus regain - foreground the game
  (game-shot does) before any capture that must show gameplay pixels.

### Exit breadcrumb: absent on ANY live-session exit (resolved to a benign property, s38)

With a LIVE XR session at exit, the `shutdown: DLL_PROCESS_DETACH` breadcrumb does NOT appear -
observed on the sim (Infinite twice, BS1 once) AND on the user's real VDXR exit (WM_DESTROY
noted, no detach line). Session 37's clean detaches were SESSIONLESS (VDXR retry mode), so the
correct statement is: **Infinite's exit is TerminateProcess-class whenever an XR session is
live, on any runtime** (dllmain's own doc: no detach + no fault = TerminateProcess/fail-fast).
In effect it is benign - prompt exit (4-6 s), no fault logged, no dump, user saw no hang -
so it is recorded as a property, not a defect. Mechanism unattributed (game exit path vs a
runtime DLL); if it ever matters (e.g. a teardown-ordering bug hides behind it), attribute it
during the release (I11) soak.

### Headset verdict (VDXR lane, user, 2026-08-05)

Quest 3 via Virtual Desktop (`VirtualDesktopXR` 1.0.10): head-tracked big screen, "looks
pretty good, no crashes or freezes/hangs". The log confirms the F10 A/B ran live on VDXR
(`session teardown (disabled in overlay)` -> `session running` ~5 s later), alt-tab survived,
two boots. SteamVR lane deferred by the user (no Steam Link hardware; carried on the release milestone, I11 post-restructure).

### BS1 shared-tool proof run (fixed sim, BS1's shipped v0.7.0 mod via -AllowStale)

Full lane: `xrsim-launch -Game bs1` -> `boot.ps1 -Attach` -> `smoke.xrs` PASS (FOCUSED, quad,
meanLuma 11.7, nonBlack 49.6 %) -> `vrstereo on` -> projViews=2, eyeSep 0.063 exact,
**claimRatioH 1.018 - identical to the session-37 geometric baseline**, proving the sRGB fix
moved only pixel statistics. New post-fix stereo baseline: left-vs-right mean-abs-diff
**11.53** (pre-fix scale read 3.16), pct-channels-changed 42.75 %.

## LIVE RESULTS (session 37 - I2 CLOSED: the five remaining DRs, all measured)

Session 37 first merged `main` (BS2 v0.7.0, 124 commits) into the Infinite line - see the
merge commit for the re-run BS1/BS2 inertness proofs - then ran the remaining battery on the
merged build. Every verdict below is a measured downstream effect from a running
`BioShockInfinite.exe` (v0.7.0-25-g2b6e76a), never a return value.

### THE TRANSFORM QUESTION: GetPlayerViewPoint is a RAW COPY on path 2. CLOSED.

The path-aware heartbeat (fixed at the end of session 36) read in real gameplay AND at the
intro: path census still 100 % path 2, comparison bound to the camera POV `[cam+0x3B8]`, and
`returned-minus-source d=(0.000 0.000 0.000)` on every beat. **The returned view is handed back
unchanged - no transform is applied between the camera object's POV fields and the out-params.**
Consequence for I4: a pose written to the camera POV (or substituted in the detour's out-params)
IS the view; there is no downstream transform to fight. The `[this+0x430]` 4x4 read all zeros at
first fire and is not on the observed path.

### DR-I4 native stereo: NEGATIVE, confirmed live. CLOSED.

Live pool lookups against the populated GNames (69,719 entries): `Stereo`, `Stereoscopic3D`,
`EyeSeparation`, `StereoDevice`, `bStereoEnabled` all **absent**. `AllowNvidiaStereo3d` IS
present (index 4154) - which is simultaneously the positive control proving the search works and
the confirmation that the only stereo surface is the driver-side 3D Vision allow-flag
(`=False` in the shipped ini). With the offline evidence (0 of 2647 natives match `*Stereo*`,
no stereo strings in the exe), there is **no usable engine-side per-eye render path**. I6 builds
its own stereo, as planned.

### DR-I5 render substrate: threads separate under BOTH lever positions. RECORDED.

- Game and render threads are SEPARATE, re-measured this session in both configurations:
  `OneFrameThreadLag=True` (camera tid 7136 vs present tid 18952 in one boot, 12808/21704 in
  another) and **`OneFrameThreadLag=False` - the topology does not change**, the game boots,
  loads and plays normally. The lever is ACCEPTED by the engine but does not single-thread it.
- Camera rate at the same save spot: ~1000 calls/s (2560x1440, lag on) vs ~2600 calls/s
  (1600x1200, lag off) - CONFOUNDED by the resolution change, recorded but not attributed.
- Substrate shape: uploads go through `UpdateSubresource` (90 M lifetime calls by mid-session),
  per-draw b0 rewrites, no kick-and-wait stall ever observed at dispatch rates up to 9681
  calls/s. Everything points at a buffered/ring submission rather than BS1's handshake -
  **whether the lag lever actually shortens the camera-to-present latency needs the I6-era
  latency instrument**; that half stays open into I6 and is not needed before it.

### DR-I6 the Exec seam: PASS by effect, through ProcessEvent by name. CLOSED.

`bsicall <Func> [float]` and `bsiexec <console cmd>` (src/game/bioshockinf/reflect.cpp) dispatch
on the latched APlayerController via `FindFunction` (vtable `+0x54`, impl `0xD1030` interlock)
then `ProcessEvent` (`+0x7C`). Gates: build gate, game-thread identity, GNames populated,
vtable-slot RVA match; SEH-isolated, fail-soft. Measured results:

| probe | result |
|---|---|
| `bsicall FOV 120` | FindFunction -> **null** - the class chain does not carry stock `FOV`. The miss lane works; nothing was called. |
| `bsiexec shot` | ConsoleCommand resolved and ran; the engine **created `My Games\...\XGame\ScreenShots\` at the dispatch timestamp** (12:33:29) - a filesystem side effect = the console chain executes. No PNG is written by this build's capture path. |
| `bsiexec setres 2560x1440` | **THE BACKBUFFER PHYSICALLY RESIZED within 20 ms** - `ResizeBuffers: 2560x1440 hr=0x0` from our own hook. The seam is live end-to-end, and unlike BS1, `setres` does not fault. |
| `bsiexec set XGamePlayerController FOVAngle 130` | dispatch completed; the rendered lens stayed at the slider-max 0.8769/0.4933 - **no frustum effect**. FOVAngle is independently known to be disconnected from the frustum (the ini lies), so this cannot separate "set is dead" from "the property is dead". A live-property retest is I5 work if ever needed; the seam itself is proven above. |
| `bsiexec get ... FOVAngle` | returned string empty - but the FString ReturnValue offset assumes stock `(string, optional bool)`; if Irrational dropped `bWriteToLog` the capture reads the wrong slot. Return-string capture is UNVERIFIED; never rely on it (verify by effect anyway). |

### DR-I7 Scaleform HUD: fingerprint CONFIRMED live, twice. CLOSED.

The session-36 offline fingerprint reproduced in a live gameplay frame with an active HUD, at a
different resolution (1600x1200) and scene: the HUD is a contiguous end-of-frame run on the
BACKBUFFER (this frame ~46 draws - the count varies with HUD content, the structure does not),
after the full-screen scene blit, from EXACTLY the two engine call sites `0x492284` (DrawIndexed,
counts divisible by 6) and `0x4920FF` (Draw), sampling **BC3 atlases (fmt 77)** T71-T77 plus an
R8 1024x1024 glyph atlas (fmt 61). The tonemap target this frame was **T8** (1600x1200
R8G8B8A8_UNORM RT|SRV) where session 36's frame used T9 - **the index varies, so the POSITIONAL
rule is confirmed as the only correct one: the eye image source is the `srv0` of the last
full-screen `DrawIndexed a=6` into the backbuffer** (event 00463: `rtv0=T0 srv0=T8`). That
texture is HUD-free by construction; no classifier is needed. The HUD has no offscreen movie
target of its own - it draws straight onto the backbuffer.

### DR-I8 resolution lever: PASS on BOTH levers - and the config layer was misidentified. CLOSED.

- **`XEngine.ini` `ResX`/`ResY` is a boot-time DERIVED COPY, not the store.** A write there is
  silently overwritten at boot (measured: wrote 1600x1200, backbuffer stayed 2560x1440 and the
  file read back 2560). The authoritative store is **`XUserOptions.ini` `ResolutionX`/
  `ResolutionY`** - writing 1600x1200 there produced `first Present: backbuffer 1600x1200`,
  which is the acceptance. (Fourth instance of "a verified write is not an honoured one".)
- **The exec lever works live**: `bsiexec setres 2560x1440` resized the backbuffer mid-session
  (ResizeBuffers logged, hr=0). Infinite's `setres` does NOT fault the way BS1's did.

### Incidental live findings worth keeping

- **Unattended attract/menu hangs the game** (pre-existing, NOT ours, NOT the merge): left
  sitting at the menu/attract with no input, the game froze twice - once on the merged build,
  once on the UNMODIFIED session-36 build (the control that exonerated the merge). The pace
  watchdog photographed both: present thread and game thread parked in ntdll waits under
  game-code frames, all 64 threads reported, zero mod frames on the wedged stacks. An attended
  menu has never hung. Sessions should keep a driver at the menu and note this for the release (I11) soak
  tests.
- **Infinite exits CLEANLY via WM_CLOSE** - `DLL_PROCESS_DETACH (orderly process exit)`, twice,
  with no exit-path fault. Better behaved than both remasters.
- Camera hook totals after the battery: 1.49 M calls in one boot, still **0 foreign-tid
  dispatches** across every boot this session.

### The FOV law (aspect cross-check): VERTICAL-referenced. The DR-I3 leftover is CLOSED.

Slider at maximum, same save, two backbuffer aspects, `-ScanMatrix -BlockBytes 80`:

| | 2560x1440 (16:9) | 1600x1200 (4:3) |
|---|---|---|
| tanH | 0.8770 | 0.6577 |
| tanV | **0.4933** | **0.4933 - PINNED** |

tanH moved by exactly **0.75000** = the aspect change; tanV did not move at all. **The option
sets the VERTICAL half-angle and tanH = tanV x aspect (Hor+).** Slider range in tanV:
0.4317 (min) to 0.4933 (max), i.e. vFOV 46.67 to 52.63 deg at any aspect. I5's lens math and
config UI derive from tanV; `FOVAngle` in the ini stays decorative.

## LIVE CONFIRMATION (session 36, second half - the game was finally free)

Everything in the two sections below was measured inside a running
`BioShockInfinite.exe`. Where a live number contradicts an offline one, the live number wins and
the offline claim is corrected in place.

### DR-I2: the camera hook FIRES. PASS.

| measurement | result |
|---|---|
| install | `prologue and ret 8 both verified`, at T+0.036 s, before the first Present |
| first fire | `this=3D4C3800 tid=13120`, during the intro/attract sequence |
| dispatch rate | **9681 calls/s** peak during the intro, settling to **~800/s** parked. BS2 sees ~850/s, so the ceiling here is an order of magnitude higher and **the fast path must stay tiny** |
| lifetime | 317,737 calls with **`foreign-tid-calls=0`** - a single dispatching thread |
| **thread split** | **camera tid 13120, present tid 1992 - SEPARATE game and render threads.** Free DR-I5 evidence |
| handover | `[cmd] status: pump=game (present=armed gameThread=OWNS)` - the seam's own status, not a log echo |

**Path census: 40 of 40 samples took path 2** (`[this+0x248]` bit 0 CLEAR, `[this+0x240]`
NON-NULL). So `+0x240` is promoted from **inferred** to **observed**: it really is a live,
lazily-created camera object, and the cached-POV fast path was never taken in this sample.

**The lease positive control passed in both directions, on demand:**

```
22:26:11.533  bsicam off
22:26:14.534  [cmd] game-thread pump silent for >3000 ms - Present pump RESUMING in DEGRADED mode
22:26:16.538  [cmd] status: pump=render(degraded)      <- and this command DISPATCHED, which is the point
22:26:21.537  bsicam on
22:26:21.539  [cmd] game thread resumed - Present pump standing down again
22:26:25.537  [cmd] status: pump=game
```

Without the lease, `bsicam off` would have bricked the command seam for the life of the process.

**One instrument defect found and recorded rather than papered over.** The heartbeat's
`returned-minus-cached` line compares the returned location against `[this+0x24C]`, the **cached
POV** - which is path 1's source. Under path 2 the POV comes from `[cam+0x3B8]` instead, so the
comparison is against the wrong field and its `identical, this path is a raw copy` verdict is
**not evidence about the transform**. The transform question is therefore still OPEN, and the
comparison needs to be path-aware before it means anything. Also noted: `[this+0x430]` read as
**all zeros** at first fire, which is consistent with the 4x4 not being populated that early.

**The motion test PASSES.** With the user driving: **12 distinct positions**, and one slow 360-degree
turn swept yaw from **-32392 to +32640 = 65032 units**, which is **99.2 % of a full 16-bit range**.
That single motion falsifies both open assumptions at once - the field at that offset really is
yaw, and 65536 rotator units really is one full turn. Roll also went non-zero (`-8`), so all three
components are live and distinct.

**FRotator is SIGNED**, and this matters for the I4 write: the values wrap into roughly
`[-32768, +32767]`, not `[0, 65535]`. A naive unsigned read would be wrong for half the circle.
Print with `%d` and convert with `* 360.0 / 65536.0`; never reinterpret the int32 as a float (the
result is a denormal that prints as `0.000`, the trap that cost BioShock 1 a long detour).

Observed dispatch rates, for the I4 budget: **9681/s** peak during the intro, **~3600/s** idle in
gameplay, **~1150/s** in a quieter scene. All far above BS2's ~850/s.

### DR-I1 confirmed live, and the table shape corrected

`bsireflect selftest`: **12 passed, 3 failed, and all three failures were one wrong assertion of
mine, not a broken instrument** (see below).

| control | result |
|---|---|
| 4 positive resolutions | **PASS** - `0x129280`, `0x1292C0`, `0x12BF30`, `0x4FC060`, all exact |
| negative, prefix | **PASS** - `strings=1 terminatorRejects=1`, rejected by the terminator guard |
| negative, pooled suffix | **PASS** - `strings=1 tableRefs=0`, rejected by the reference step |
| negative, absent | **PASS** |
| `GNames[0] == "None"` | **PASS**, with `Num=69718`. The UE3 `fname_text` works on a populated pool |
| `fname_find("None") == 0` | **PASS** |
| UClass fixpoint | **PASS** - `Class=13FA4508`, `Class->Class = ->Class = 0A4DFA80` |
| FNameEntry wide path | **UNTESTED, reported as such** - 0 wide entries in the first 4096, so the UTF-16 branch is not proven working. Not counted as a pass |

**The vtable read confirms the offline derivation exactly:**

```
+0x54 [21] rva 0xD1030   <== FindFunction   (offline predicted slot +0x54)
+0x7C [31] rva 0x19A150  <== ProcessEvent   (offline predicted slot +0x7C)
+0x80 [32] rva 0xD9960
```

Slot `+0x7C` on an `APlayerController` holds **`AActor::ProcessEvent`**, exactly as predicted for
an AActor subclass rather than the `UObject` base at `0xCFE70` - which is the prediction only the
base/override split made possible. `0xD1030` is `UObject::FindFunction`, immediately preceding
`FindFunctionChecked` at `0xD1090`. And slot `+0x80` is `0xD9960`, the by-name dispatch helper
disassembled during the offline pass (it calls `FindFunctionChecked` then `[edi+0x7C]`).

**CORRECTION - the native registry is NOT one contiguous 2647-entry table.** The walk returned 46
entries, which looked like a failure and was not. Reading the live-seeded region shows the layout
is the classic `AutoRegisterNatives` shape: **one block per class, separated by `{0, 0}` sentinel
entries.**

```
0xF32560  {0,0}                                      <- sentinel
0xF32568  APlayerControllerexecClientControlMovieTexture  0x129730
   ...    (44 more)
0xF325C0  APlayerControllerexecGetPlayerViewPoint         0x129280   <- seed index 11
   ...
0xF326D8  {0,0}                                      <- sentinel
0xF326E0  AGameReplicationInfoexecFindPlayerByID          0x1138A0
```

**46 is exactly `APlayerController`'s native count from the session-34 per-class census.** The
2647 figure is the total across every class and was never a contiguous run. So
`native_table_bounds` returns the seed's **own class block**, which is still a falsifiable
per-class check - and the table-walk cross-check must be seeded from the class it is about to look
up, since a block from another class cannot contain the name. Fixed in code and in the selftest
assertion.

### DR-I3: the lens is FOUND

`dumpframe cb` in gameplay wrote **7.1 MB** (against 104 KB for a lite dump) with **1891 constant-
buffer uploads** captured, in tiers `80 B x231, 160 B x156, 640 B x186, 1280 B x493, 2560 B x788,
3360 B x37`. Both of the instrument's blind spots were real and are now covered: the **160-byte
tier** (156 uploads) that `hud_capture`'s `>= 320` gate always excluded, and the **3360-byte** tier
that the old 336-float cap would have truncated at 40 %.

Sweeping every offset of every block for an orthogonal 4x4 gives **139 candidates**, and filtering
on the aspect discriminator leaves exactly **one**:

| | |
|---|---|
| location | **float 0 of the 80-byte constant buffer**, row-major |
| tangents | **tanH = 0.7674, tanV = 0.4317** |
| aspect | **1.7776** vs the backbuffer's 1.7778 - 0.01 % |
| rendered FOV | **hFOV 75.01 deg**, vFOV 46.67 deg |
| support | 93 blocks, **identical in both consecutive dumps** |

The aspect filter is what makes this credible, and the raw histogram shows why: the top four
candidates by block count are all degenerate `tanH == tanV` pairs at aspect 1.0000, one of them
with 108 blocks - more support than the true answer's 93. **Plurality is not evidence here; the
aspect match is.** That is the same failure mode the BS1 control predicted.

#### CONFIRMED by the falsifiable prediction - DR-I3's lens question is CLOSED

Moving the in-game FOV slider from minimum to maximum and re-dumping moved that block, and only
that block, exactly as a projection must:

| | tanH | tanV | hFOV | vFOV | aspect |
|---|---|---|---|---|---|
| slider min | 0.7674 | 0.4317 | 75.01 deg | 46.67 deg | 1.77762 |
| slider max | 0.8770 | 0.4933 | 82.50 deg | 52.63 deg | 1.77782 |
| **ratio** | **1.14282** | **1.14269** | | | held at 16:9 |

**Both axes scaled by the same ratio to five significant figures, with the aspect preserved to
0.002 %.** That is the signature of a FOV change and of nothing else - a view or object term could
not do it, and a coincidence could not do it twice on the same float offset in the same byte tier.
Same offset, same layout, same tier across all three dumps.

**A CONFIG VALUE IS A CLAIM, NOT A MEASUREMENT - a third instance, and this one nearly poisoned
I5.** Session 34 read `FOVAngle=70` plus `MaxUserFOVOffsetPercent=15` as "the native slider spans
roughly 70 to 80.5 degrees". The rendered frustum spans **75.01 to 82.50 degrees**. Neither
endpoint matches, and the measured tangent ratio of **1.1428** is nowhere near the **1.2094** that
a 70-to-80.5 span predicts. **I5 must derive the FOV law from the frustum, not from the ini** - and
still at two aspects, since one aspect cannot separate the horizontal and vertical conventions.

The constant lives in `src/game/bioshockinf/patterns.h` as `kLensCbBytes` / `kLensFloatIndex` /
`kLensRowMajor`. It is deliberately **NOT** published through `hud::set_ray_block_offset`: core's
`decode_ray_block` encodes the Vengeance 7-float shape and cannot consume a 4x4, so pointing it at
float 0 would fail or, worse, false-positive. A 4x4-shaped live decoder behind the same
`hud::fov_watch` API is I5 work.

**Still owed (small):** the aspect cross-check at a second backbuffer size, which also settles
DR-I8 for free via `first Present: backbuffer WxH`.

## DR-I2: the camera seam - hook WRITTEN, live confirmation OWED

The read-only detour is implemented in `src/game/bioshockinf/camera.cpp` and builds clean. It has
**not been observed firing** - BioShock 2 held the machine for the whole of session 36's coding
half, and this project's rule is that a hook is not real until it is seen to dispatch. The bit
`CAP_CAMERA_OVERRIDE` is therefore wired to `has_fired()`, not to a successful install: a
deliberate divergence from BS1 and BS2, which both key it off the install.

What the install refuses on, all fail-soft (log and let the game run flat):

- build gate closed (`patterns::rva_trusted()`);
- the 12-byte live prologue at `0x1E10C0` not matching
  `55 8B EC 83 E4 F0 81 EC A4 00 00 00`;
- **no `C2 08 00` (`ret 8`) in the first 0x400 bytes.** This is an independent confirmation of the
  argument count *before* the detour is created, because getting it wrong pops the RTC dialog that
  writes no crash dump. One typedef serves both the trampoline pointer and the detour so the two
  cannot disagree about arity.

Read-only by construction: the out-params are copied into `const` locals and **no assignment
through them exists anywhere in the translation unit**. The I4 write seam will be a new function,
not a loosening of this one.

Measurements the hook is built to take on its first live run, each answering an open question:

| measurement | question it settles |
|---|---|
| first-fire line, then sustained `calls/s` at the menu **and** in gameplay | does the seam exist in both states; what is the dispatch budget |
| path census (`[this+0x248]` bit 0, `[this+0x240]` non-null) | promotes the **inferred** `+0x240` to observed. Paths 3 and 4 are ONE bucket on purpose - separating them needs a virtual call through slot `+0x2C0` out of a detour, which can lazily create engine objects |
| one-shot 16-float dump of `[this+0x430]`, plus per-beat `returned-minus-cached` delta | is the returned view really **transformed**, or is the fast path a raw copy - measured, not assumed |
| camera tid vs `d3d11_hook::last_present_tid()` | UE3's game/render thread split, which is free DR-I5 evidence. Deliberately on the throttled path, not the first-fire line: the hook installs at ~T+0.4 s and the first Present lands at ~T+8.5 s, so `last_present_tid()` can still be 0 when the detour first runs |

### The command pump is now a LEASE, not an eviction (core change, session 36)

The moment the detour fires it calls `bvr::command::poll_from_game_thread`, moving the command
poller off the render thread - the whole reason the core seam was built pump-agnostic in session 35.

That handover used to silence the Present pump **permanently**, on the reasoning that a
resume-on-stall rule would hand the render thread a dispatch during a load, "the worst moment
available". The hazard is real but was stated too broadly: what must not happen during a load is an
**engine-touching** dispatch, not any dispatch. As written, a camera hook that went quiet (level
load, Scaleform menu, scripted camera) left the mod with no command surface **and no line saying
so**.

So `poll_from_game_thread` now stamps a timestamp on every call, and after a 3 s lease expires the
Present pump resumes in **degraded** mode: `mempoke*`, `pokeaddr*` and `memrestore` are refused with
one explanatory line, everything else dispatches. Both transitions are logged.

**This is inert for BS1 and BS2 by construction**, verified by grep rather than assumed: neither
adapter includes `core/framework/command.h`, neither calls `enable_present_pump()` or
`poll_from_game_thread()`, and `poll_from_present` returns immediately unless an adapter armed it.
The only callers anywhere in the tree are `bioshockinf`'s.

It also made the handover **testable on demand**, which the original was not: `bsicam off` silences
the detour body deliberately, `vrcmd` must then report `pump=render(degraded)`, and `bsicam on` must
hand back. An instrument that cannot be made to fail is not evidence.

### The command seam is core and Present-driven on this game

`%LOCALAPPDATA%\BioshockVR\bsi\command.txt` is polled at 1 Hz **from the Present detour**, not from an
engine hook, so it worked from the first frame with `capabilities() == 0` and no engine hook
installed anywhere. Two consequences specific to Infinite:

- **Commands run on the RENDER thread** while the Present pump owns the poller. Long scans
  (`memscan`, `fsweep`) stall presents rather than the game thread. When I2's camera hook lands it
  calls `command::poll_from_game_thread`, which latches a **one-way** handover and silences the
  Present pump for the life of the process.
- **A pre-existing `command.txt` is skipped at startup**, not executed - the first poll adopts the
  file's write time and logs what it ignored. This is the trap TESTING.md records as having bitten
  BS1 three times. BS1 and BS2 still run their own pollers and still have the old behaviour.

## DR-I3: the frame map - PASS offline, the lens question is still OPEN

### The deferred pass order, derived from the banked session-35 lite dump

No game needed for any of this. Reconstructed by walking the `SetRT` sequence and attributing each
draw, its viewport and its VS b0 size tier to the pass that was bound.

| ev | target | dsv | draws | b0 tier | what it is |
|---|---|---|---|---|---|
| 00002 | T1 `R8G8B8A8_TYPELESS` | T2 | 1 | 640 | depth/G-buffer prime |
| 00011 | **T9 `2560x1440 R8G8B8A8_UNORM RT\|SRV`** | T2 | **26** | **1280** | **main G-buffer opaque pass** (4 RTVs) |
| 00102 | T21 `R32_FLOAT` | - | 1 | 1280 | depth linearise (`srv0=T2`) |
| 00114-00146 | T28/T30/T31/T34/T35 `128x96`, `128x288` | - | 10 | 160 | bloom / eye adaptation chain |
| 00162 | - | **T39 `2048x2048 R32_TYPELESS`** | 1 | 640 | shadow atlas, `vp=2038x2038` |
| 00169-00181 | T40, T1 | T2 | 3 | 160/640 | pre-lighting (`srv0=T21`) |
| **00186** | **T41 `2560x1440 R11G11B10_FLOAT`** | T2 | 4 | **160** | **deferred lighting** (`srv0=T9`,`T40`) |
| **00206** | **T22 `2560x1440 R16G16B16A16_FLOAT`** | T2 | **41** | 640x29, 1280x11, 2560x1 | **main HDR scene pass** |
| 00382 | T57 `256x16` | - | 16 | 160 | colour-grading LUT build (16 slices) |
| 00402-00438 | T58/T59 `1280x720`, T60-T65 `640x360` | - | 6 | 160 | bloom chain |
| **00440** | **T9** | T2 | 1 | 160 | **tonemap resolve** (`srv0=T50`) |
| **00444** | **T0 backbuffer** | - | 1 | 160 | **final blit**, `DrawIndexed a=6 srv0=T9` |
| 00451 | T0 | T2 | **9** | 160 | **Scaleform HUD**, `srv0=T66`/`T67` |

Formats seen: `10` R16G16B16A16_FLOAT, `26` R11G11B10_FLOAT, `27` R8G8B8A8_TYPELESS, `28`
R8G8B8A8_UNORM, `39` R32_TYPELESS, `41` R32_FLOAT, `44` R24G8_TYPELESS, `77` BC3_UNORM.
Bind flags: `0x20` RT, `0x28` RT|SRV, `0xA8` UAV|RT|SRV, `0x48` SRV|DSV.

### TRAP: T9 is REUSED, so the BS1 tonemap rule cannot work here

T9 is bound as `rtv0` **three times** - it is the G-buffer albedo target at event 11 *and* the
tonemap output at event 440. `hud_capture`'s `g_curRtLdr` predicate (R8G8B8A8_UNORM, large enough,
RT|SRV) matches T9, T36 and T38 alike, so **a descriptor-based rule cannot pick the tonemap target
on this game**. Any Infinite rule must be **positional**: *the `srv0` of the last full-screen draw
into the backbuffer*. That is event 00449, `DrawIndexed a=6 srv0=T9`.

### Free half-answer for DR-I7 (Scaleform), recorded now so it is not re-derived

The HUD is a contiguous run of **9 draws at the very end of the frame, on the backbuffer, AFTER the
tonemap blit**, sampling BC3 atlases (T66 `2052x620`, T67 `4x16`), with index counts all divisible
by 6 and exactly two engine call-site RVAs: `0x492284` (DrawIndexed) and `0x4920FF` (Draw). This is
structurally cleaner than the remasters' gameswf: **T9 is HUD-free by construction**, so the eye
image can be taken from T9 with no classifier at all.

### Instrument gap: the dump records rtv0 only

Every world `SetRT` on this game binds **4 RTVs**, and the dump records `rtv0` alone. On a deferred
renderer that discards three quarters of the frame map. `OMSetRenderTargetsDetour` already receives
the full array; only the recording throws it away. Worth fixing before I6.

### The lens: a scoped NEGATIVE, and why the instrument was rebuilt

**The experiment has already been run once, and it failed with a statable scope.** Core's live FOV
watch is `CopySubresourceRegion`-based and it *was* actively sampling on Infinite - the banked dump
shows 8 staging copies per frame at `a = 0, 1344, 2688 ... 9408` (exactly `kFovCbBytes * slot`) from
3 distinct source buffers. Across **19,602 presents** it never adopted an offset, and
`decode_ray_block` brute-forces after 400 consecutive misses and adopts after 8 corroborating hits.

So the negative is real and precisely bounded: **no `(2tanH, 0, -tanH, 0, 0, -2tanV, tanV)` block
exists in floats 0..335 of any VS b0 buffer of >= 320 bytes.** It has exactly three holes, all in
code we own:

1. `hud_capture.cpp` gates the watch on `bd.ByteWidth >= 320`, and **Infinite's deferred lighting
   pass uses the 160-byte tier** - 52 of the 130 sized draws in the banked frame. The pass that most
   needs an inverse-projection constant is the one the gate has always filtered out.
2. `kFovCbBytes = 1344` truncates the 2560-byte tier.
3. It is VS-only.

**And a plain `dumpframe full` would not have closed them.** `frame_inspector` gates its cb0
readback on the VS b0 **buffer object changing**, which is right for a Map/WRITE_DISCARD engine that
renames its buffer per upload. Infinite reuses a handful of objects and rewrites them with
`UpdateSubresource` (15.3 M lifetime calls, **251 in a single frame**), so mode 2 emits a block only
at object transitions and then attributes many draws to a block whose contents were overwritten in
between. That is worse than an empty dump, because it looks like data.

### What was built instead (session 36)

- **`dumpframe cb` (frame_inspector mode 3)** captures every `UpdateSubresource` into a constant
  buffer, **at its real size**, from the call parameter - no staging buffer, no `Map` stall, no
  readback race, every stage and slot, and the 160-byte tier included. Plus per-draw VS/PS
  constant-buffer identities (the first `PSGetConstantBuffers` call in the codebase). Strictly
  additive: modes 1 and 2 are untouched, so BS1 and BS2 cannot move.
- **`decode-framedump.ps1 -ScanMatrix`.** UE3 ships a 4x4, not a Vengeance 7-float ray block. For
  row-vector `M = W*V*P`, column 3 is forward times object scale `s`, `|c0| = s/tanH` and
  `|c1| = s/tanV`, so **`tanH = |c3|/|c0|`, `tanV = |c3|/|c1|` and the object scale CANCELS** -
  which is what makes it work on a per-object constant buffer where nothing else is constant.
- The old `-ScanLayout`/`-Diff` hardcoded 336 floats in three places. The worst was a block
  terminator that truncated any buffer over 1344 bytes and then **silently dropped** the remaining
  continuation lines, producing a plausible wrong block. All three are gone.
- `-BlockBytes` (restrict to one cb size tier - essential when b0 is per-object and the modal value
  at most indices is object noise), `-MinModeShare`, `-DiffAspects` (an independent diff axis where
  one of the two tangents is PINNED, and the identification is the cross-product with `-DiffFovs`),
  and `-SelfTest`.

### The controls, run before any of it is trusted

| control | result |
|---|---|
| `-SelfTest`: plant a known lens in a synthetic block | **PASS** - both decoders recover tanH/tanV exactly, with the object scale cancelling |
| `-ScanLayout` regression on a BS1 dump (known answer: offset 12) | **PASS** - still offset 12, tanH 1.1918 / tanV 1.2351 across 133 blocks. The count-agnostic rewrite broke nothing. |
| `-ScanMatrix` cross-check on the same BS1 dump | **PASS and independent** - finds `f40` transposed, tanH **1.1917** / tanV **1.2350**, matching the ray block to four decimals by a completely different decode. It finds BS1's *second* lens too (0.6470/0.6706 vs the ray block's 0.6468/0.6704). |
| `-ScanMatrix` on a BS2 dump where `-ScanLayout` finds NOTHING | **PASS, and this is the new capability** - that dump is the 2048x2048 square-aspect case that cost BS2 a session. `-ScanMatrix` recovers tanH = tanV = 0.6703, i.e. 68.4 deg at aspect 1.0, against the 67.7 deg BS2's notes record for that configuration. |

**Honest limitation of `-ScanMatrix`, found by its own control and recorded rather than papered
over:** it produces false positives on degenerate matrices. On the BS1 dump, `f22` transposed with
tanH=0.5000 / tanV=1.0000 scored **156 blocks and outvoted the true answer's 83**. So *plurality of
blocks alone is NOT sufficient* - the aspect cross-check is load-bearing, and a candidate is only
believable when `tanH/tanV` matches the backbuffer aspect. Suspiciously round pairs (0.5/1.0,
1.0/1.0) are the tell.

**Still owed, and it needs the game:** a `dumpframe cb` in gameplay, then `-ScanMatrix` first,
`-ScanLayout` second, then the FOV and aspect diffs. Until then the cb0 ray-block offset for
Infinite is **UNKNOWN**, `bioshockinf` publishes no `set_ray_block_offset`, and core's default of
12 (a BioShock 1 fact) is what would be used - which is precisely why nothing consumes it yet.

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

## The native function table EXISTS (session 34, offline, and it is the best news so far)

BS1's single fastest instrument was the engine's name-based native function table, which resolves
natives with **zero hardcoded addresses** and found BS1's aim seam in minutes. The concern was that
UE3 favours indexed natives and the lane would be lost. **It is not lost.**

| | BioShock 1 (Vengeance) | BioShock Infinite (UE3) |
|---|---|---|
| entry | 12 bytes `{ const TCHAR* name; Native impl; 0 }` | **8 bytes `{ const ANSICHAR* name; Native impl; }`** |
| name encoding | UTF-16 | **ASCII** |
| name form | `int<Class>exec<Function>` | **`<Class>exec<Function>`** (no `int` prefix) |
| count | 1822 | **2647** |
| location | `.data`, names in `.rdata` | same |

Enumerated offline with `scratchpad/dump-natives.ps1` (read-only, disk image; the dump is
game-derived and stays in the scratchpad - only the findings come here). Top classes by native
count: `AXPlayerController` 231, `AXPawn` 188, `UObject` 179, `AActor` 104, `AXWeapon` 68,
`APlayerController` 46, `USkeletalMeshComponent` 43, `UXGFXMovie` 26.

**Runtime resolution is therefore the same recipe as BS1**: find the ASCII name string, find the
dword that references it, read the *next* dword. Relocation-transparent, patch-tolerant, no
hardcoded address. `pattern_scan::find_native_function` needs only an ASCII/8-byte-stride variant.

**Caveat that cost BS1 real time and applies verbatim here:** the linker pools string literals by
suffix, so a substring match proves nothing. Require the null terminator at the expected end.

### Exec thunks are NOT the seam - independently reproduced on Infinite

BS1 hooked all four aim `exec` thunks and caught **zero** calls across a live session of shooting,
because native C++ callers bypass the script thunks entirely. The same is true here, and it was
measurable offline before any hook was installed. Static `E8` caller census over `.text`:

| target | E8 callers |
|---|---|
| `APlayerController::execGetPlayerViewPoint` (thunk, RVA `0x129280`) | **0** |
| `APlayerController::execXGetPlayerFloatingViewPoint` (thunk, RVA `0x1292C0`) | **0** |
| `APawn::execGetBaseAimRotation` (thunk, RVA `0x12BF30`) | **0** |
| `AXPlayerController::execCalcFOV` (thunk, RVA `0x4FC060`) | **0** |
| **`APlayerController::GetPlayerViewPoint` (IMPLEMENTATION, RVA `0x1E10C0`)** | **14** |

One of those 14 is the thunk itself at `0x1292B3`, so 13 are native call sites. **Hook
implementations, not thunks** - and the thunk is how you *find* the implementation: disassemble it,
and the last `call` before the epilogue is the real function.

## The camera seam (derived offline, session 34 - NOT yet confirmed live)

`APlayerController::GetPlayerViewPoint`, **implementation RVA `0x1E10C0`**, reached from the thunk
at `0x129280`.

- Signature: `thiscall`, `this` in `ecx`, **2 stack args** `(FVector* out_Location,
  FRotator* out_Rotation)`, `ret 8`.
- **`ret imm / 4 == 2`, so any probe hook takes 2 args.** Getting this wrong pops the RTC dialog
  that writes no dump (see the rules preamble).

Decoded control flow, which is the single most useful thing to know before writing the hook. It has
**four** paths, and only the first is a cheap cached read:

1. `test byte ptr [this+0x248], 1` -> if clear, copy `[this+0x24C]` (FVector) and `[this+0x258]`
   (FRotator) straight to the out-params and return. A **cached POV** fast path.
2. Otherwise, `[this+0x240]` holds a lazily-created camera object (created via `0x111360`,
   initialised via `0x1107E0`). If non-null, the POV is read from `[cam+0x3B8]` (FVector) and
   `[cam+0x3C4]` (FRotator).
3. Otherwise a virtual call through **vtable slot `+0x2C0`** (this is `GetViewTarget()`) returns an
   actor, and the POV is read from `[actor+0x44]` / `[actor+0x50]`.
4. Otherwise it falls back to the controller's own `[this+0x44]` / `[this+0x50]`.

Then all four paths converge at `0x1E11C8`, which copies **0x40 bytes from `[this+0x430]`** and
runs a full **4x4 SSE matrix multiply** against the POV. So the returned view is a *transformed*
result, not a raw field read - budget for that when injecting an HMD pose.

**Two layout facts fall out of paths 3 and 4**, which use identical offsets on two different
objects, which is what makes the reading credible rather than a guess:

- **`AActor::Location` at `+0x44`** (FVector, 12 bytes)
- **`AActor::Rotation` at `+0x50`** (FRotator, 3x int32, 12 bytes)

**Confidence.** The RVAs, the `ret` imm, the caller counts and the offsets *as read by this
function* are structural facts from the binary. The *names* attached to them (`+0x240` is the
camera, slot `+0x2C0` is `GetViewTarget`) are inference from shape and must be confirmed live in
I2. Nothing here has been observed executing yet.

## RTTI is present but USELESS here (confirmed negative, session 34)

BS1 and BS2 both lean on an RTTI walk (`.?AVClassName@@` TypeDescriptor -> COL+12 -> vtable-4) to
name classes and resolve vtables. **That lane is dead on Infinite.**

The exe has 270 RTTI type descriptors, and **not one of them is a UE3 engine or XGame class.**
Checked explicitly and all absent: `UObject`, `AActor`, `APlayerController`, `UEngine`, `UWorld`,
`ULevel`, `UClass`, `UFunction`, `APawn`, `UCanvas`, `UGameEngine`, `XConsole`, `FSceneView`,
`FViewport`, plus every `XPlayer`/`XGame` spelling.

What the 270 actually are: third-party libraries compiled with `/GR` - Wwise audio (`CAk*`, ~150 of
them), Bullet physics (`bt*`), FaceFX (`Fx*`), Beast/JRT lightmapping, and `std::`. UE3 itself is
built `/GR-` because it has its own reflection.

**Consequence:** class identification must come from the native table (above) and from
`GNames`/`GObjObjects` enumeration, not from RTTI. That is a real loss relative to the remasters,
and it is exactly why the native table existing matters so much.

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

### The live user config confirms it survived the template (session 34, after the first launch)

The shipped `XGame\Config\Default*.ini` files are *templates*. What the engine actually reads is
`%USERPROFILE%\Documents\My Games\BioShock Infinite\XGame\Config\X*.ini`, generated on first
launch. So the real question was never "is it in DefaultInput.ini" but "did it survive into
XInput.ini". **It did**, verified by reading the generated file:

- Every debug bind is present in the live `XInput.ini`: `Delete`=`god`, `End`=`preventdeath`,
  `PageDown`=`walk`, `PageUp`=`ghost`, `Backslash`=`behindview`, `F1/F2/F3`=`viewmode ...`,
  `F9`=`shot`, `F7/F8`=`set D3DRenderDevice bUsePostProcessEffects False/True`.
- **`[Engine.Console]` in the live file carries `ConsoleKey=Tilde` and `TypeKey=Tab`** (lines
  220-222). Neither key appears in the shipped `DefaultInput.ini` - the engine merged them in from
  `Engine\Config\BaseInput.ini`. **This is the single biggest quality-of-life difference from the
  remasters, where the console is compiled out and key-bound commands are inert.** If the console
  opens, the entire `Exec`-seam apparatus BS1 needed becomes optional rather than mandatory, and
  test loadouts stop being a mini-saga.

### VERDICT: the key-bind lane is DEAD. The reflection lane is not. (I0, session 34, user-tested)

**All six binds did nothing.** Console (`~` and `Tab`), `PageUp` ghost, `F1` wireframe, `F9` shot,
`F7`/`F8` post-process, `Delete` god - every one inert, verified in-game by the user.

Corroborated rather than taken on trust: `F9` = `shot` should drop a file, and there is **no
screenshot anywhere** - not in `My Games\...\Binaries\Win32` (the directory exists and is empty),
not in the game folder, not in Steam's screenshot userdata. The instrument was seen not to fire.

**So the config was a claim, and the claim was false.** This is the same lesson BS1 and BS2 each
learned the expensive way, arriving here for free: a shipped ini entry proves a key is *wired*, not
that anything is *listening*. The templates were never stripped because an ini is data, not code.

**But the diagnosis is narrower and much better than "cheats are gone", and the difference matters:**

| evidence | reading |
|---|---|
| `SETRES`, `FULLSCREEN`, **`SHOT`** all present as UTF-16 `Exec` literals in the exe | the C++ `Exec` handlers **are compiled in**. `shot` exists; pressing F9 simply never reached it. |
| `GOD`, `GHOST`, `WALK`, `SLOMO`, `VIEWMODE`, `BEHINDVIEW` absent as literals | expected - in UE3 these are UnrealScript exec functions on `CheatManager`, not C++, so they live in the `.u` package and would never appear here. Their absence is **not** evidence of removal. |
| **`UXCheatManager` exists** as a class with registered natives | Irrational's cheat manager is **in the shipped build** |
| `ConsoleCommand` registered as a native on `AActor`, `APlayerController`, `AXPlayerController` (impl RVA `0x136070`) and `UGameViewportClient` (`0x137150`) | **there is a reflection-callable console entry point**, independent of any console UI |

**The broken link is input dispatch, not the commands.** Infinite uses `XCore.XPlayerInput` with a
custom binding parser (the shipped ini documents its own approved `XInputHandler` chain mechanism),
and that parser evidently does not forward arbitrary console strings in the retail build.

**Consequence for the plan.** Infinite lands in the same place as BS1 and BS2 - the mod must issue
commands itself rather than through a key - but with a materially better hand, because these are
*registered natives with known implementation addresses* rather than addresses to be hunted:

| want | native | **exec thunk** RVA (NOT the implementation - see below) |
|---|---|---|
| run any console command | `APlayerController::ConsoleCommand` | `0x136070` |
| god mode | `AXPawn::AddInvulnerableFlag` / `RemoveInvulnerableFlag` / `HasInvulnerableFlag` | in the dump |
| give a weapon | `AXPawn::SetWeapon` | `0x4F9ED0` |
| give ammo | `AXWeapon::AddAmmo` | `0x5017D0` |

> **CORRECTED session 36.** These were recorded as "impl RVA". The static caller census says all
> three have **0 `E8` callers**, which is the signature of an exec thunk, not an implementation -
> the native table's second dword is always the thunk. Calling one directly means constructing an
> `FFrame`. Since DR-I1 landed `ProcessEvent` (`0xCFE70`, `ret 0xC`) and `FindFunctionChecked`
> (`0xD1090`), **the test loadout should go by name through ProcessEvent instead**, which is BS2's
> design and needs no further addresses.

So a test loadout does not even require the console: `SetWeapon` + `AddAmmo` + `AddInvulnerableFlag`
by reflection is a more direct route than the console ever was, and it sidesteps whatever gates
`bCheatsEnabled`. **This is I2 work** - it needs the adapter to exist first.

**Confidence:** the natives and their RVAs are structural facts from the binary. That calling them
from injected code works, and that nothing gates them, is unproven. Verify by effect.

> **PROVEN, session 37.** `ConsoleCommand` resolved BY NAME off the latched controller and run
> through `ProcessEvent` executes for real: `setres` resized the backbuffer (our ResizeBuffers
> hook logged it), `shot` created its ScreenShots directory at the dispatch timestamp. See
> "LIVE RESULTS (session 37)" above; the commands are `bsicall` / `bsiexec`.

### Save data location

Saves are **not** under `My Games`. They live in Steam cloud-synced userdata:
`C:\Program Files (x86)\Steam\userdata\<steamid>\8870\remote\SaveData\`, as `.sav` plus a `.bkm`
bookmark sibling, named `<MapTag>-0_<M_D_YYYY_H_M_S>_<n>.sav` (e.g. `Light_Top-...` for the opening
lighthouse, `TWN-...` for Columbia town). Back one up before any destructive test - and note Steam
Cloud will re-sync, so a "deleted" save can come back.

## Renderer, resolution and FOV (config-level findings, unverified live)

### Frame pacing levers

| setting | file:line | shipped value | why it matters |
|---|---|---|---|
| `OneFrameThreadLag` | `Engine\Config\BaseEngine.ini:726` | `True` | Setting this `False` removes one frame of render-thread lag. This is a **config-level analogue of BS1's `reentry 1t`**, obtained without hooking a flush point. If it works, it deletes the single most expensive rabbit hole on BS1 (sessions 5-8). |
| `bSmoothFrameRate` | `BaseEngine.ini:192` | `TRUE` | Frame smoothing must be off for VR. |
| `MinSmoothedFrameRate` / `MaxSmoothedFrameRate` | `BaseEngine.ini:193-194` | 22 / 124 | |
| `RenderThreadJobQueuePriority` | `XGame\Config\DefaultEngine.ini:456` | 7 | |
| `AllowD3D11` | `BaseEngine.ini:733` | `True` | |

### Live values after the first launch (session 34)

Read from the generated user config, so these are what the engine is actually running with:

| key | file | value | note |
|---|---|---|---|
| `ResX` / `ResY` | `XEngine.ini:877-878` | **2560 / 1440** | ~~matches `XUserOptions.ini` `ResolutionX/Y`, so `XEngine.ini` is the resolution lane~~ **CORRECTED session 37: the match is because `XEngine.ini` is a boot-time COPY of `XUserOptions.ini`.** A write to `ResX/ResY` is silently overwritten at boot (measured). The lever is `XUserOptions.ini` `ResolutionX/ResolutionY` - honoured at first Present. DR-I8 closed on that lane. |
| `DisplayMode` | `XUserOptions.ini:152` | **0 = windowed** | good news for the harness - `PrintWindow` capture and synthetic clicks behave far better windowed than exclusive fullscreen |
| `bSmoothFrameRate` | `XEngine.ini:152` | **FALSE** | already off. The shipped `BaseEngine.ini` says TRUE; the generated config disagrees, so one VR blocker is pre-cleared. |
| `MaxSmoothedFrameRate` | `XEngine.ini:154` | 120 | moot while smoothing is off |
| `OneFrameThreadLag` | `XEngine.ini:808` | **True** | still on. This is the config-level `reentry 1t` candidate - the lever to flip in DR-I5. |
| `AllowD3D11` | `XEngine.ini:814` | True | renderer still needs live confirmation (D3D11 vs the D3D9 path) |
| `AllowNvidiaStereo3d` | `XEngine.ini:157` | False | consistent with the "no engine per-eye path" reading |
| `FieldOfView` | `XUserOptions.ini:138` | 0.000000 | the native slider at its default, i.e. base FOV with no offset |
| `FOVAngle` | `XEngine.ini:502` | 70.0 | |

### Resolution

- `XGame\Config\DefaultUserOptions.ini`: `DefaultResolutionX` / `DefaultResolutionY` = `-1`
  ("use the current system settings default, will be saved out as its own version after that
  point"), `DefaultDisplayMode=1` (0 windowed / 1 fullscreen / 2 fullscreen-windowed).
- `XGame\Config\DefaultEngine.ini:219-220`: `ResX=1280`, `ResY=720`.
- `SETRES` exists as a UTF-16 exec string in the exe. On BS1 the equivalent **faulted** through the
  viewport Exec seam, which is why the ini lane became primary there. **Tested session 37: it
  WORKS live** - `bsiexec setres 2560x1440` resized the backbuffer within 20 ms, ResizeBuffers
  hr=0. Infinite has BOTH a working ini lane and a working live lane.
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

### The audited retail pad map (s42, shipped DefaultInput.ini == live XInput.ini, verified)

The complete `XboxTypeS_*` binding set the synthetic pad must serve (chains abbreviated to
their effect; `A + B` = XInputHandler short-circuit chain, `| OnRelease X` = release action,
`| FlashCommand X` = the Scaleform/UI-context alternative):

| pad control | bound chain (effect) |
|---|---|
| A | TBar transfer activates (highlight on press, perform on release) + **Jump**; air-to-TBar window |
| B | TBar dodge + TBar reverse + **ToggleCrouch** |
| X | **ReloadOrHoldToHackOrUse** / FlashCommand ActivateFirstNeedle |
| Y | TBar melee transfer + **melee attack** (hold 0.15 s = execution) / AbortHack |
| LT | XStartTBarZoom + **StartFirePlasmid** (release: stop both) |
| RT | **Fire** |
| LB | **NextPlasmid** / ActivateSecondNeedle |
| RB | **NextWeapon** / ActivateThirdNeedle |
| LS click | **StartSprint** |
| RS click | **XToggleZoom** |
| DPad Up | XNavShowPulse / BuyoutHack |
| DPad Down | XMakeUnstableSelection / AutoHack |
| DPad L/R | XNavQuickToggleCycleLeft/Right |
| Start | ShowPauseMenu; Back | XUserInterface_OnBackButtonPressed |
| LX/LY/RX/RY | `Axis aLeftStickX/aLeftStickY/aRightStickX/aRightStickY Speed=1.0 DeadZone=0.0` |

AxisEmulationDefinitions map stick extremes to `Gamepad_LeftStick_*` button events (menu
navigation). The live file also carries a merged BaseInput default set (aStrafe/aTurn/
aLookup, deadzone 0.2-0.3) for a different input class - the XGame set above is the one the
game plays with. Keyboard notes for parity: PC keys use SwapWeapon/SwapPlasmid (two-slot
carry) where the pad uses Next*.

**Consequences for the per-game pad map (s43)**: core's `input_sync` BioShock-1 semantics
are WRONG here on four counts - the B->Y / Y->B face-button re-route (Infinite wants
straight-through: A jump, B crouch/dodge, X use/reload, Y melee), the ammo-slot dpad
machinery (RS-click is deliberately consumed on BS1 but Infinite NEEDS it for XToggleZoom,
and synthesized dpad pulses mean hack/nav commands here), grips->bumpers (fine - bumpers =
Next weapon/plasmid, a good Touch fit), and the menu long-press->BACK (fine - back button).
The extraction must be additive/opt-in (adapter-supplied map; absent = today's hardcoded
path, BS1/BS2 byte-identical) per the decoupling directive.

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
| `DLC\DLCA` | 5.8 GB | **Clash in the Clouds** - identified session 34 from package names: `DCLA_ZEP_Wave1..14`, `DLCA_Arc_BlueRibbons`, `DLCA_ARMORY_WEAPONS` (wave arena, Blue Ribbon challenges, the armory) |
| `DLC\DLCB` | 7.5 GB | **Burial at Sea Ep. 1** - `DLCB_BookersOff` (Booker's office), `DLCB_Attrium`, `DLCB_Appliances`, `DLCB_Arc_Chameleon` |
| `DLC\DLCC` | 11.8 GB | **Burial at Sea Ep. 2** - the remaining story pack, largest of the three; carries `Columbia_Billboards_DLCC` and its own subtitle font set |
| `BirdsEye`, `ChinaBroom`, `IndustrialRevolution`, `SeasonPass`, `UpgradePack` | ~1 MB each | entitlement stubs |

Burial at Sea adds weapons and a Vigor (Old Man Winter) the base game does not have, so per-weapon
aim profiles must cover them.

---

# Derivation recipes

Short form. The full versions, with worked examples and the traps, are in
[../bioshock1/ENGINE_NOTES.md](../bioshock1/ENGINE_NOTES.md) and
[../bioshock2/ENGINE_NOTES.md](../bioshock2/ENGINE_NOTES.md).

- **Static caller census - run this BEFORE hooking anything.** `tools/pe-xref.ps1` (committed
  session 36; the earlier sessions used throwaway scratchpad scripts, which is exactly why their
  numbers could not be reproduced). For any absolute-addressed function, scan the exe for `E8`
  opcodes whose rel32 lands on it. Zero callers on a function the engine "must" call every frame
  means the dispatch is inlined, virtual or dynamic. This is the check that cracked BS2's dead-thunk
  mystery after a hook was installed that never fired.
  Its `AbsRef` mode counts 4-aligned dwords equal to `ImageBase+rva`, histogrammed by section, which
  is the **vtable detector**: a virtual has FEW `E8` callers and MANY `.rdata` dwords, and that
  inversion is itself the identification. `-FollowStubs` catches callers routed through an `E9`
  jump stub - without it a census can report a false zero, which is the most expensive kind of wrong
  answer here. **Write the predictions down before running it**, and require the tool to reproduce a
  known number first; on Infinite that control is `GetPlayerViewPoint 0x1E10C0 == 14 callers`.
- **Function start without a prologue heuristic (session 36).** Collect every `E8` call target in
  `.text`; that set is the complete list of entry points anyone calls, so "the function containing
  X" is "greatest target <= X". Use this instead of `find_function_start`'s
  `CC CC CC 55 8B EC` backward walk, which on this exe returns the **previous** function rather
  than failing, because Infinite has frameless functions.
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
| `IDXGISwapChain::Present` | vtable slot | frame boundary, XR pacing, overlay, mirror, **the command poller** | kiero-style throwaway device (core, game-agnostic) | **LIVE (session 35)** - installs at T+0.4 s, first Present T+8.5 s |
| `IDXGISwapChain::ResizeBuffers` | vtable slot | RT cache invalidation | same | **installed (session 35)**, not yet observed firing |
| context Draw/DrawIndexed/SetRT/Map/Unmap | context vtable | frame dumps, HUD classification | core `frame_inspector`, game's own immediate context | **LIVE (session 35)** - 15/15 slots, one dump taken |
| `UObject::ProcessEvent` | **impl RVA `0xCFE70`**, vtable slot **`+0x7C`** | script-event seam; the route to the test loadout and to named-event work | FindFunctionChecked's callers -> slot histogram -> vtable-candidate histogram (session 36, offline) | **derived, `ret 0xC` (3 args). Not hooked.** |
| `AActor::ProcessEvent` | impl RVA `0x19A150` | the override an AActor subclass actually carries in slot `+0x7C` | same histogram's runner-up; confirmed by its tail-call into the base | derived, not hooked |
| `UObject::FindFunctionChecked` | impl RVA `0xD1090` | resolve a `UFunction*` by FName before a ProcessEvent call | UTF-16 `Failed to find function` xref -> enclosing function via the E8-target set | **derived, `ret 0xC` (3 args), 426 callers. Not hooked.** |
| **`APlayerController::GetPlayerViewPoint`** | **impl RVA `0x1E10C0`** (thunk `0x129280`) | **the camera seam**: 6DoF override. thiscall, 2 stack args, `ret 8`, 13 native callers | native table -> disasm thunk -> last call before epilogue -> caller census | **READ-ONLY detour WRITTEN (session 36), prologue- and `ret 8`-gated. NOT yet observed firing - live confirmation owed.** |
| viewport draw root (SR doubling) | `0x1FDE30` | SequentialReentry stereo seam (camera + scene + present per call) | live caller census + stack scrape + vtable probes, capstone confirm (s40) | **HOOKED, verified live s40** |
| client draw (camera loop) | `0x26A3E0` | derivation link only - doubling it yields NO second present (recorded negative, s40) | vtable `0xDE6FC8` slot +0x8 -> stub `0x6F1360`, live probe | verified live s40, not hooked |
| `XInputGetState` | game IAT RVA `0xCD4814` (XINPUT1_3 ord 2) | synthetic gamepad | import table parse (**done**, session 34) | confirmed, unhooked |
| Draw / DrawIndexed / OMSetRenderTargets / Map+Unmap | context vtable | HUD classification, frame dumps, lens watch | core `frame_inspector` | pending I2 |
| engine `Exec` dispatchers | - | console commands by code | RTTI + vtable walk | pending I2 |

# Symbol / offset table

All from session 34, offline, read-only. **Nothing here has been observed executing yet** - every
row is a structural fact from the disk image plus, where noted, an inference from shape.
Everything must also live in `src/game/bioshockinf/patterns.h/.cpp` and nowhere else once used.

## Functions (RVA, image base `0x00400000`, ASLR off)

| symbol | RVA | notes |
|---|---|---|
| `APlayerController::GetPlayerViewPoint` **(impl)** | `0x1E10C0` | thiscall, 2 stack args, `ret 8`, 13 native callers, **0 absolute refs (not in any vtable)**. **The camera seam.** |
| **`UObject::ProcessEvent`** | **`0xCFE70`** | thiscall, **3 stack args, `ret 0xC`**. Vtable slot `+0x7C`. 1 E8 caller, 2144 `.rdata` refs - the signature of a virtually-dispatched function. |
| **`AActor::ProcessEvent`** | **`0x19A150`** | thiscall, `ret 0xC`. Thin override that tail-calls the base. 0 E8 callers, 268 refs. This is what an AActor subclass carries in slot `+0x7C`. |
| **`UObject::FindFunctionChecked`** | **`0xD1090`** | thiscall, **3 stack args, `ret 0xC`**. 426 E8 callers (the generated event stubs), 0 absolute refs. |
| `APlayerController::execGetPlayerViewPoint` (thunk) | `0x129280` | 0 callers - do not hook |
| `APlayerController::execXGetPlayerFloatingViewPoint` | `0x1292C0` | Irrational addition; 0 callers |
| `APlayerController::execXGetMatineeViewTarget` | `0x129240` | of interest for cinematics (I9 post-restructure) |
| `APlayerController::execSetViewTarget` | `0x1291A0` | |
| `APlayerController::execGetFOVAngle` | `0x1290A0` | |
| `ACamera::execGetFOVAngle` | `0x127A00` | |
| `ACamera::execAdvanceFOV` | `0x127910` | |
| `ACamera::execSetViewTarget` | `0x127BB0` | |
| `AXPlayerController::execCalcFOV` | `0x4FC060` | Irrational's own FOV path - the first FOV lever to try |
| `AXCamera::execCalcFOVSpeed` | `0x503AA0` | |
| `UXPostProcessingEffect::execCalcFOV` | (in dump) | a *third* FOV consumer - assume multiple lenses until disproven |
| `APawn::execGetBaseAimRotation` | `0x12BF30` | aim seam starting point for I8 |
| `UGameViewportClient::execGetViewportSize` | `0x124B90` | |

Note the FOV picture: `APlayerController`, `ACamera`, `AXPlayerController` and
`UXPostProcessingEffect` all have FOV entry points. **That is three or four consumers before a
single frame has been dumped**, which is exactly the multi-lens situation I5 is built to expect.

## Offsets

| object | offset | field | confidence |
|---|---|---|---|
| `AActor` | `+0x44` | `Location` (FVector) | high - two independent paths in `GetPlayerViewPoint` read it off different objects |
| `AActor` | `+0x50` | `Rotation` (FRotator, 3x int32) | high - same |
| `APlayerController` | `+0x248` bit 0 | "use cached POV" flag | structural |
| `APlayerController` | `+0x24C` | cached POV Location (FVector) | structural |
| `APlayerController` | `+0x258` | cached POV Rotation (FRotator) | structural |
| `APlayerController` | `+0x240` | camera object pointer, lazily created | **inferred** from shape |
| camera object | `+0x3B8` / `+0x3C4` | POV Location / Rotation | structural |
| camera object | `+0x3D0` | POV FOV, f32 horizontal degrees at the 16:9 reference (FTPOV: loc, rot, fov) | measured s41 - tan(deg/2) == the decoded frustum tanH at 16:9, and poking it is overwritten within a tick (per-tick recompute) |
| camera object | `+0x214` | DefaultFOV analog, f32 degrees; `+0x218`/`+0x21C` hold 1.3333f (DefaultAspectRatio pair) | measured s41 - same recompute; both copies are the FOV lever's per-dispatch write targets |
| `APlayerController` vtable | `+0x2C0` | `GetViewTarget()` | **inferred** from shape |
| `APlayerController` | `+0x430` | 0x40-byte block fed to a 4x4 SSE transform applied to the POV | structural, purpose unknown |
| `UObject` | `+0x18` | `Name` FName index dword | measured s42 - candidate walk on the PC's class object read 'XPlayerController'; derived per run by `bsifields`, text-checked |
| `APlayerController` | `+0x1FC` | `Pawn` (XHuman in gameplay) | measured s42 - bsifields class-name walk |
| `APlayerController` | `+0x23C` / `+0x240` | `Player` (XLocalPlayer) / camera object (XCamera) | measured s42 - the +0x240 inference above CONFIRMED by class name |
| `APlayerController` | `+0x340` | `PlayerInput` (XPlayerInput) | measured s42 |
| `APlayerController` | `+0x344` | `CheatClass` (UClass; the manager instance is NEVER spawned) | measured s42 |
| image | `0xCD4814` | XINPUT1_3 ord-2 (XInputGetState) IAT slot | s34 PE parse, VERIFIED live s42 (target resolved into XINPUT1_3.dll before the re-point) |

## Globals

| global | RVA | how derived |
|---|---|---|
| **`GNames.Data`** (`FNameEntry**`) | **`0xF9DFEC`** | traced from the hardcoded `UnNames.h` string run |
| **`GNames.Num`** | **`0xF9DFF0`** | same function, the grow path |
| **`GNames.Max`** | **`0xF9DFF4`** | same |
| **name hash table** | **`0xF58BF8`** | `mov [eax*4 + 0xF58BF8], ebp` after `and eax, 0xfff` -> **4096 buckets** |
| **`GNatives`** (bytecode opcode dispatch) | **`0xF6DCB0`** | `movzx edx, byte ptr [eax]` (opcode from the script stream) then `mov edx,[edx*4 + 0xF6DCB0]; call edx` in every exec thunk |
| **`GMalloc`** | **`0xF71CC8`** | `mov ecx,[0xF71CC8]; mov eax,[ecx]; mov edx,[eax+8]; call edx` = vtable Malloc |
| `__security_cookie` | `0xF4BD60` | standard MSVC prologue |
| empty-string constant | `0xECD640` | substituted whenever a string's length field is 0 |

## Data tables

| table | location | shape |
|---|---|---|
| native function registry | `.data`, names in `.rdata` | 2647 x 8-byte `{ const ANSICHAR* name; Native impl; }`, names `<Class>exec<Func>` ASCII |
| `GNames` | `0xF9DFEC` | UE3 `TArray<FNameEntry*>`, the classic 12-byte `{ Data, Num, Max }` triple |

## Struct layouts

**`FNameEntry`** - decoded from the pool allocator and the registration function:

| offset | field |
|---|---|
| `+0x8` | **`(index << 1) | isWide`** - the index is `[entry+8] >> 1`, and **bit 0 selects ASCII vs UTF-16 text** (two different hash functions are called on it, `0x80C70` vs `0x80C10`) |
| `+0xC` | hash-chain next pointer |
| `+0x10` | **the name text**, ASCII or UTF-16 per the bit-0 flag |

**This differs from BS1 in a way that will bite a naive port.** BS1's `FNameEntry` text at `+0x10`
is always UTF-16; here it is **ASCII by default** (the allocator copies 5 bytes for `"None"`) with
wide as the exception. Any `fname_text()` must read the flag, not assume an encoding.

**`FFrame`** (the script execution frame passed to every exec thunk):

| offset | field |
|---|---|
| `+0x14` | `Object` (the `UObject*` the bytecode is running on) |
| `+0x18` | `Code` (instruction pointer into the bytecode stream; thunks `inc` it as they parse) |

**`UObject`**: `Class` at **`+0x20`** (from `execIsA`: `mov eax,[edi+0x20]`).

**`UClass`**: carries two `WORD`s at **`+0xC0`** and **`+0xC2`** used for a **constant-time `IsA`**
via an interval/nested-set test (`execIsA` compares `child.start >= parent.start` and
`child.start <= parent.start + parent.range` with 16-bit math, no vtable walk, no loop). Worth
reusing rather than walking `SuperStruct` chains.

## Not found (recorded so it is not re-hunted blindly)

- **`GObjObjects`** - the linear global object array. `UObject::StaticFindObject` (`0xC6250`) goes
  through the **object hash**, not a linear walk, so the array does not fall out of it. Unresolved
  globals seen on that path: `0xF8BF04`, `0xF79D30`, `0xF83EA0` - **not** identified, do not guess.
  **Deprioritised on purpose**: BS2's design acquires live objects from *hook parameters* (the
  `this` of the camera hook, the `Object` field of an `FFrame`) rather than by scanning, which is
  both cheaper and safer - BS1's object scanner caused a 3.8 s game-thread stall and probably a
  crash. Pick this up only if a use case appears that hook parameters cannot serve.

# Dead ends

Recorded here as they happen, with the address, so nobody re-walks them. BS1's list saved real
time; start this one early.

- **RTTI walk - DEAD.** 270 type descriptors exist but every one belongs to a third-party library
  (Wwise `CAk*`, Bullet `bt*`, FaceFX `Fx*`, Beast/JRT, `std::`). Zero UE3 or XGame classes. UE3 is
  built `/GR-`. Do not try to name an engine class this way; use the native table or
  `GNames`/`GObjObjects`. *(Session 34, offline.)*
- **`exec` thunks - not a seam.** Confirmed by static caller census: 0 `E8` callers on every thunk
  checked. Native C++ callers bypass them entirely, exactly as on BS1. Use them to *locate* the
  implementation, never as a hook target. *(Session 34, offline; re-confirmed session 36 with
  `tools/pe-xref.ps1`, which also showed `ConsoleCommand`/`SetWeapon`/`AddAmmo` are thunks too.)*
- **`pattern_scan::find_event_function` - DO NOT USE on this game.** Three independent reasons, all
  fatal: it searches UTF-16 only; it ends in `find_function_start`'s `CC CC CC 55 8B EC` backward
  walk, which cannot see a frameless function and silently returns the **previous** one rather than
  failing; and on BS2 the same call already produced dead code. The bsi lane decodes **forward**
  from an xref instead, which sidesteps the problem entirely. *(Session 36.)*
- **UE3 script-VM error strings - mostly absent.** `Runaway loop detected`, `Infinite script
  recursion`, `Missing native function`, `Stack overflow`, and the literal symbol names
  `ProcessEvent` / `FindFunctionChecked` / `ProcessInternal` / `CallFunction` /
  `ProcessRemoteFunction` do not exist in the exe in either ASCII or UTF-16. Only
  `Failed to find function` and `Accessed None` survive the link. Absence is not evidence of
  removal - a shipping build folds most `appErrorf` format strings. *(Session 36, offline.)*
- **`call [reg+disp]` is the WRONG shape to search for a UE3 virtual dispatch.** MSVC hoists the
  vtable pointer into a register, so the code reads `mov edi,[esi]` ... `mov edx,[edi+SLOT]` /
  `call edx`. Searching for `FF /2` found 0 of 426 event stubs. Any decoder must track register
  loads and resolve `call reg` through them. *(Session 36 - cost one wrong answer before it was
  caught by disassembling a caller instead of trusting the histogram.)*
- **Native stereo - no script surface.** Zero natives match `*Stereo*` across all 2647 entries, and
  no `bStereo`/`EyeSeparation`/`StereoDevice` names exist in the exe. Combined with
  `AllowNvidiaStereo3d=False` in the ini, the shipped "Stereoscopic3D" is almost certainly
  driver-side 3D Vision. Plan for SequentialReentry. *(Session 34, offline - still worth one live
  confirmation in DR-I4, but do not budget hope for it.)*
- **Key-bound console commands - DEAD, user-tested.** All six shipped debug binds (`~`/`Tab`
  console, `PageUp` ghost, `F1` wireframe, `F9` shot, `F7`/`F8` post-process, `Delete` god) do
  nothing in the retail build, despite all being present in the live `XInput.ini` and despite
  `ConsoleKey=Tilde` being set. Confirmed by positive control: `F9`=`shot` produced no screenshot
  anywhere. The custom `XCore.XPlayerInput` binding parser does not forward console strings.
  **Do not spend another session on key binds, launch flags or ini edits to enable the console.**
  Go through `APlayerController::ConsoleCommand` (native, impl RVA `0x136070`) or straight to the
  gameplay natives. *(Session 34, user-verified in-game.)*
