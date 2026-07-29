# Engine notes - BioShock 2 Remastered (Bioshock2HD.exe)

Single source of truth for everything we know about **Bioshock2HD.exe** internals.
Sibling of [../bioshock1/ENGINE_NOTES.md](../bioshock1/ENGINE_NOTES.md) (BS1), which also documents
the general derivation recipes in full; this file records the BS2-specific values and the places
where BS2 *differs* - and it already differs at the very first hook.

Rules (same as BS1):
- Every address/offset used by code lives ONLY in `src/game/bioshock2r/patterns.h/.cpp`, and every
  one is documented here with its derivation method.
- NEVER copy a number from the BS1 notes or patterns - same engine tree, different link. Shapes
  transfer; values never do.
- No game-derived content in the repo: findings are summarized here, disassembly stays out.

## PE identity (verified 2026-07-29, session 24)

| field | BioShock 1 | BioShock 2 |
|---|---|---|
| machine | x86 (0x14C) | x86 (0x14C) |
| LARGE_ADDRESS_AWARE | yes | yes |
| ImageBase | 0x10900000 | 0x10900000 (identical) |
| SizeOfImage | 0x1677000 | 0x1FEA000 (bigger) |
| PE TimeDateStamp | 2022-04-13 16:16:54 UTC | 2022-04-13 16:00:37 UTC |
| imports xinput1_3.dll / d3d11.dll | yes / yes | yes / yes |

BS2 was built 16 minutes BEFORE BS1, same engine, same build session. The `xinput1_3.dll`
proxy injection vector works unchanged (verified live session 24: `build:`/`env:` lines in
`%LOCALAPPDATA%\BioshockVR\bs2\bioshockvr.log` on first launch). Steam appid 409720. Game path:
`D:\SteamLibrary\steamapps\common\BioShock 2 Remastered\Build\Final\Bioshock2HD.exe` (D: drive,
unlike BS1 on K:). Observed live base 0x10040000 (rebased - ASLR active, RVAs are the stable
identifiers, exactly as on BS1).

An important build difference with consequences everywhere: **BS2's link went through an
incremental-link style jmp-stub table** (RVAs ~0x0D000-0x40000: five-byte `E9` stubs). Vtable
slots and many call sites point at stubs, not bodies - every derivation must follow one `E9` hop
(`follow_jmp_stub` in patterns.cpp). BS1 has no such indirection. Additionally BS2's optimizer
**inlined code BS1 kept out-of-line** - see the CalcView seam below.

## The CalcView seam (THE session-24 finding)

**`eventPlayerCalcView` exists in BS2 and is DEAD CODE.** The FName-chain scan resolves it
cleanly (RVA 0x395CC0, correct thunk shape, 0x1C param block), the hook installs - and it never
fires, menu or gameplay. Offline byte-scan: BS1's thunk has **29 static E8 callers; BS2's has
ZERO.** The build inlined the event-dispatch glue at every call site (~35 sites reference the
cached FName index global vs BS1's 3).

Each inlined site does, verbatim from capstone disasm of sites at RVAs 0x4D6970 / 0x868759 /
0xA092CF / 0x34D139:

1. build the 0x1C param block on the stack: `{ AActor* viewActor; FVector loc; FRotator rot }`
2. load the 8-byte FName `{ index, number }` from the cached-index global (dwords at
   RVA 0x17D9A08 / 0x17D9A0C) - **BS2's FName constant is 8 bytes** where BS1 passed 4
3. `push 0` (UBOOL global) + the FName -> call `UObject::FindFunctionChecked` (callee pops 12)
   through the jmp stub at RVA 0x20365 -> real body at **RVA 0xB6BA30**
4. push the returned `UFunction*` -> virtual call through **vtable slot 3 (byte +0xC)** =
   `ProcessEvent(UFunction*, void* parms, void* result)` - `ret 0xC`, three stack args
5. read the param block back and consume loc/rot

So the working BS2 seam (shipped in `bioshock2r/camera.cpp`) is:

- **hook `FindFunctionChecked` (RVA 0xB6BA30)** - when `nameIndex == *(RVA 0x17D9A08)`, cache the
  returned `UFunction*`. Zero UObject-layout assumptions; the camera sites re-resolve the name
  every dispatch, so the cache is fresh from the first frame.
- **hook `ProcessEvent` (RVA 0x37A7E0)** - after calling the original, if `fn` matches the cached
  pointer, mutate `parms` (the caller reads the block back afterwards). Equivalent to BS1's
  out-param writes.
- ProcessEvent is resolved at runtime by reading the AShockPlayerController vtable slot +0xC and
  following its stub; the result must equal base+0x37A7E0 and match the documented prologue, or
  the adapter refuses to hook (build-identity gate).

| symbol | RVA | derivation |
|---|---|---|
| `eventPlayerCalcView` (dead) | 0x395CC0 | FName-chain scan, resolved live; kept for the record only |
| PlayerCalcView FName index global | 0x17D9A08 (+0x17D9A0C number) | FName-chain scan (1 wide string, 1 string xref, ctor RVA 0x19C04, 37 global xrefs) |
| `UObject::FindFunctionChecked` | 0xB6BA30 | inlined call sites -> stub 0x20365 -> body; prologue `55 8B EC 64 A1 00 00 00 00` (SEH frame) |
| `UObject::ProcessEvent` (outer) | 0x37A7E0 | controller vtbl slot 3 stub 0x15FBE -> body; prologue `55 8B EC 8B 81 0C 01 00 00`; `ret 0xC` |
| ProcessEvent vtable slot | byte +0xC (slot 3) | read off the inlined call sites (`call [edx+0xC]`) |

Cross-check: BS1's outer ProcessEvent sits at RVA 0x375140 with the IDENTICAL shape (StateFrame
gate at `this+0xF8` there vs `this+0x10C` here, script-disable globals, tail-jmp to the inner
body, `ret 0xC`) - located via BS1's controller vtable slot 3. Same engine, shifted offsets:
never copy, always re-derive.

**ProcessEvent hook discipline:** every script event in the game passes through the detour.
Pre-filter work is two compares; the 1 Hz command-file poll ticks through a `& 0xFF` call
counter. Observed dispatch rates: ~200 calls/s of PlayerCalcView at the loading/menu-adjacent
scenes, ~850/s in gameplay, spikes to ~4500/s during level load. Total ProcessEvent traffic is
far higher - keep the fast path tiny.

## Class vtable RVAs (RTTI walk, runtime-verified)

Derivation: MSVC RTTI walk (find `.?AVClassName@@` TypeDescriptor -> the dword referencing its VA
is COL+12 -> the dword referencing the COL's VA is vtable-4), run offline 2026-07-29; the method
was validated by reproducing all of BS1's known-good vtable RVAs exactly.

| class | RVA | runtime verification (session 24) |
|---|---|---|
| `AShockPlayer` | 0x11197C0 | **VERIFIED** - view actor vtable in gameplay matches; drives `view state: GAMEPLAY` |
| `AShockPlayerController` | 0x1117BF0 | **VERIFIED** (indirectly) - its vtable slot 3 stub resolves to ProcessEvent, and the pre-save menu/load view actor logs it |
| `UShockUserSettings` | 0x11523D8 | candidate - unconsumed (FOV readback is the next step) |
| `APlayerWeapon` | 0x112CC78 | candidate - unconsumed |
| `AHands` | 0x1125478 | candidate - unconsumed |
| `SkeletonInstance` | 0x10D0FC0 | candidate - unconsumed |
| `UGameEngine` | 0x10BD7DC / 0x10BD9E8 | candidates - unconsumed (BS1's console_exec used the second of its pair; verify before calling) |

Runtime self-diagnosis: the camera module logs the observed view-actor vtable RVA on every
change, so a wrong candidate names its own correction from any session log. Observed so far:
0x106EE20 - the MENU/base controller class (BS2's main menu runs CalcView with
`viewActor == pc` of a NON-Shock controller class; identity not yet RTTI-resolved).

## Behavior differences from BS1 worth remembering

- **The main menu does not run PlayerCalcView at all** until a level/menu scene spins up a
  controller (first fire arrives seconds after launch with the 0x106EE20 controller). BS1's
  attract scene fires CalcView at up to ~7800/s from the start. Consequence: nothing driven from
  the CalcView tail works at the BS2 menu - which is why the b2r command poller ticks from the
  ProcessEvent detour instead.
- The graphics-options first-boot screen has a native **Field Of View slider (default 100)** -
  BS2 has an exposed FOV concept BS1 lacked; likely the same `UShockUserSettings` int the BS1
  FOV write targets. Verify offset fresh before writing.
- Flat 6DOF checks (session 24, log-measured via the final-camera heartbeat): `offset 0 0 50` ->
  z +50.0 exact; `simhead 0 20 0` -> pitch 3640; roll 15 -> 2730; yaw residual integer-exact
  (16450 -> 10989 = -5461 = -30.0 deg); sim position (0.10, 0.20, -0.30) m at worldscale 100 ->
  headOff (6.1, 31.0, 20.0) UU, |37.4|, halving exactly at worldscale 50.

## Derivation recipes (shared with BS1 - short form)

- **FName-chain event scan** (core `pattern_scan::find_event_function`, game-agnostic): wide
  UTF-16 event name -> imm32 xref -> forward <=96 bytes to the `E8` FName-ctor call -> past it to
  the `89 0D` store = cached index global -> global xrefs (minus init site +-200) -> backward
  prologue walk `CC CC CC 55 8B EC`. On BS2 remember the chain's first PROLOGUE-VALID candidate
  is not necessarily a LIVE function - check callers before trusting a resolved thunk.
- **RTTI walk**: see the table intro above; ~40-line offline script, disposable (scratchpad).
- **Static caller census**: for any absolute-addressed function, scan the exe for `E8` opcodes
  whose rel32 lands on it. Zero callers on a function the engine "must" call per-frame = the
  dispatch is inlined or dynamic - look for the FName-global xrefs instead. (This is the check
  that cracked the dead-thunk mystery; do it BEFORE hooking, not after.)
- **Scan hygiene** (LAA: actors allocate above 2 GB, so any future heap scan walks the full 4 GB
  range; no scans on a cadence - BS1 needed backoff AND dormancy; prologue-gate every hook).
