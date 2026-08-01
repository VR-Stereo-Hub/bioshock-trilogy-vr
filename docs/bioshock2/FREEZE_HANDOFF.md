# BS2 hard freeze - session handoff (2026-07-31, session 34)

The whole purpose of the next session. Everything below was measured this session; nothing
here is inference unless it says so.

## The prompt for the next session

> Session 35: BS2 - fix the SequentialReentry hard freeze. Nothing else.
>
> Work on a NEW branch off `s34-b2r-pacing-and-rig` (NOT off main - main cannot see this bug;
> its logging is written by the thread that wedges, so it just goes silent). Do not merge to
> main. Suggested branch: `s35-b2r-reentry-freeze`.
>
> Read first: `docs/bioshock2/FREEZE_HANDOFF.md` (this file), then the block comment at the
> top of `src/game/bioshock2r/scenedraw.cpp` ("THE FREEZE - WHAT IT IS, AND WHAT IT IS NOT"),
> then `git log --oneline -15`.
>
> **The bug**: with `vrstereo on`, BioShock 2 hard-wedges within 5-100 seconds. Permanent -
> the process never recovers and must be killed. It is NOT a crash (no fault, no dump).
>
> **It reproduces flat, on a desk, with no headset and no OpenXR session.** You do not need
> the user for any part of the diagnosis. Do not ask them for headset time until you have a
> fix that survives a 10-minute flat soak.
>
> **Root cause, measured**: the doubled Draw blocks in a cross-thread completion handshake
> with an INFINITE timeout. See "What is actually happening" below for the disassembly.
>
> **Two fixes already tried and REFUTED - do not retry them** (see "Dead ends").
>
> Acceptance: `vrstereo on`, then 10 minutes of flat soak with zero `WATCHDOG` lines in
> `pacetrace.log` and the main log still advancing. Then, and only then, ask the user to
> confirm in the headset.

## How to produce the hang (5 minutes, no headset)

```powershell
.\tools\build.ps1
.\tools\install.ps1 -Game bs2
.\tools\launch-game.ps1 -Game bs2
```

Then:

1. Foreground the game window and press **Space** at the title screen. On this machine that
   resumes straight into gameplay at the test save.
2. Write the command file (the harness batch script is fine too):
   ```powershell
   Set-Content "$env:LOCALAPPDATA\BioshockVR\bs2\command.txt" -Value "vrstereo on" -NoNewline -Encoding ascii
   ```
3. Wait. It wedges in 5-100 s. Foreground or background makes no difference (tested both).

**Confirming it is the wedge and not something else:**

- `Get-Process Bioshock2HD` -> `Responding = False`, and it stays False forever.
- `%LOCALAPPDATA%\BioshockVR\bs2\bioshockvr.log` stops advancing and never resumes, even if
  you foreground the window.
- `%LOCALAPPDATA%\BioshockVR\bs2\pacetrace.log` KEEPS advancing (own thread, own file handle)
  and prints, once per second:
  ```
  TRACE ... presents/s 0 | phase: - | stage: - | draw: secondDraw for 18105 ms
  WATCHDOG secondDraw stuck 4218 ms tid=NNNN eip=77E499DC in ntdll.dll+0x799DC ... rva: B8108F BB1963 ...
  ```

The control, to prove a candidate fix actually did something: `vrcam on` instead of
`vrstereo on` arms the VR camera WITHOUT the doubled draw and soaks indefinitely (5 min,
zero events). `vraer on` arms AlternateEye stereo, which also never re-enters the draw.

## What is actually happening

`eip` sits in `ntdll!NtWaitForSingleObject+0xC`. The nearest game frame returns into a
thiscall wrapper that is literally:

```
push [ebp+8]                  ; timeout
push [ecx+4]                  ; HANDLE stored at +4 of an event object
call KERNEL32!WaitForSingleObject
neg eax ; sbb eax,eax ; inc eax   ; -> (result == WAIT_OBJECT_0)
ret 4
```

and its caller is:

```
mov esi, ecx                  ; this
cmp dword ptr [esi+8], 0      ; "already finished" flag
jne  skip                     ; set -> no wait at all
mov ecx, [esi+0x10]           ; event object
push -1                       ; INFINITE
call [eax+0x14]               ; virtual Wait(INFINITE)
skip:
mov ecx, [esi+0xc]
```

That is the textbook lost-wakeup shape: a wakeup delivered between the flag test and the
wait is gone forever. SequentialReentry doubles these handshakes per frame and shifts their
timing, so it is a matter of when, not if.

**It is the engine's own race. VR only makes it fire.** Reproduced identically on `main`
(build `eaac2ab` detached), so nothing session 34 changed causes it.

## Dead ends - do NOT retry

1. **PulseEvent -> SetEvent.** The wrapper sitting next to the Wait one calls
   `KERNEL32!PulseEvent`, whose documented lost-wakeup behaviour fits perfectly. Redirected
   the import to `SetEvent` (which latches), armed at init, measured: **`PulseEvent calls 0`**.
   The engine never calls it on this path. Adjacency in a binary is not a calling relationship.
2. **Bounding the INFINITE wait.** IAT-clamped `KERNEL32!WaitForSingleObject`, scoped by a
   `thread_local` to exactly the re-entered draw, 2000 ms. The freeze stops and the game
   **crashes instead** - `fault at 101E1A4B` repeated 86000 times. The caller ignores the
   wait's return value, so the timeout is not itself fatal; the engine proceeds to use a
   resource that genuinely is not ready. **A crash is worse than a freeze.** This does confirm
   that wait IS the freeze point.
3. **A foreground gate on the doubled draw.** The freeze reproduces with the window focused
   (the trace line carries `fg=1`). The alt-tab correlation was coincidence.

## Candidate fixes, in confidence order

1. **Render single-threaded while stereo is armed.** This removes the cross-thread handshake
   instead of fighting it, and it is exactly what BioShock 1 does (`reentry 1t`) - which is
   why BS1 does not hang here. BS2's equivalent has never been derived. Session 26 explicitly
   decided not to, on a premise this session refuted: its comment claims *"the Draw path has
   no submit handshake (that spin-wait belongs to the streaming manager)"*. The doubled draw
   plainly reaches a blocking cross-thread wait.
2. **Drop draw re-entrancy on BS2 and ship AlternateEye stereo** (`vraer on`, added this
   session). Real per-eye stereo, never re-enters the draw, structurally cannot hit this bug.
   Costs judder - each eye updates every other frame. Flat-stable for 150 s, but with no
   headset there is no session, so the per-eye CAPTURE path is still unproven. **One headset
   test settles whether this is a shippable fallback**, and it is the cheapest thing in this
   document.
3. Identify what signals that event and why the doubled draw misses it - i.e. find the
   partner of the wait rather than the wait. Needs the signalling call site; the watchdog can
   be pointed at it (see below).

## The instruments (all added session 34, all in this branch)

- **`pacetrace.log`** - written by a dedicated thread with its OWN file handle, opened
  `_SH_DENYWR` so it can be tailed while the game is frozen. It must never use `BVR_LOG`:
  that takes a process-global `std::mutex` and the tracer then queues behind the very stall it
  is describing. That mistake cost this session an hour of silent logs.
- **Stall detection keys on presents having stopped**, not on one of our own phases being
  open. The wedge is outside every span the VR module wraps, so a phase-based check reports
  nothing. (It also had a bug where `continue` skipped the baseline update, making the
  condition permanently false - check that kind of thing.)
- **`WATCHDOG`** - suspends the wedged thread, reads its context, scans its stack for
  return addresses inside the game image, resolves the module of `eip`, resumes. This is what
  named the bug. `watchdog_all_threads()` exists but currently prints nothing (its `nf == 0`
  filter or the snapshot silently fails) - **fixing that is probably the fastest next step**,
  because it would show the OTHER side of the deadlock.
- **Stage markers** - `set_present_stage()` across every segment of the Present detour and
  `set_draw_stage()` around the doubled draw, so the trace can say which segment the thread is
  in rather than "everything stopped".
- **`tools/disasm-rva.py`** turns those RVAs into disassembly. Its output is game-derived:
  never commit it, summarise in ENGINE_NOTES.

## What else is in this branch (unrelated to the freeze, all measured)

- **The 10 Hz pacing slowdown is fixed** - `xrEndFrame` blocks ~102 ms while unfocused, and
  eventually forever. The present thread now makes no blocking OpenXR call while the session
  is not FOCUSED. Flat A/B: 238 presents/s with it on, 1/s with it off. Separate bug from the
  freeze; do not conflate them.
- **Black bars diagnosed and fixed** - the eye is 108 x 110 deg, the game rendered 100 x 67.7,
  so 38% of the eye's height was black. BS2's FOV law makes `tanV` aspect-invariant, so the
  FOV option is the ONLY lever that adds vertical view. `vrfov` now ships default ON.
- **The helmet** - BS1's zoom-pull does NOT exist on BS2 (A/B/A dump triple: only the
  projection tangents move, the near plane holds, no eye position moves). The porthole is one
  mesh, index count **3810**, confirmed by making it disappear. Overlay toggle, default off.
