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

**Observation for I10, recorded now so it is not mistaken for a bug later:** core's letterbox
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
| **`UObject::FindFunction`** | **vtable slot `+0x54`** | virtual | read out of FindFunctionChecked's own body: `mov eax,[esi]; mov edx,[eax+0x54]; call edx`, then the null test that reaches the `appErrorf`. Recorded, not consumed. |
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
| `ResX` / `ResY` | `XEngine.ini:877-878` | **2560 / 1440** | matches `XUserOptions.ini` `ResolutionX/Y`, so **`XEngine.ini` is the resolution lane** and the options UI writes through to it. Still needs backbuffer-at-first-Present acceptance (DR-I8). |
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
| scene build / draw root | - | SequentialReentry stereo seam | frame inspector callstack RVAs + capstone | pending I2/I6 |
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
| `APlayerController::execXGetMatineeViewTarget` | `0x129240` | of interest for I11 cinematics |
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
| `APlayerController` vtable | `+0x2C0` | `GetViewTarget()` | **inferred** from shape |
| `APlayerController` | `+0x430` | 0x40-byte block fed to a 4x4 SSE transform applied to the POV | structural, purpose unknown |

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
