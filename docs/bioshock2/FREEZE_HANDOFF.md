# BS2 hard freeze - RESOLVED (sessions 34-36)

Session 34 localised it, session 35 derived the root cause, session 36 measured the trigger
question, ported the fix, and soaked it. This file is now the historical record and the repro
recipe; the engine knowledge lives in `ENGINE_NOTES.md` ("The render flush point"), and the
session-by-session narrative in `docs/STATUS.md`.

## What it was

`vrstereo on` (the SequentialReentry doubled draw) hard-wedged the game in 5-100 s: permanent,
no fault, no dump, process must be killed. Reproduced flat - no headset, no OpenXR session.

**Root cause (verified against the exe, then live):** `UGameEngine::Draw`'s tail makes exactly
one call to a render flush point (RVA `0x69FC30`) whose threaded branch is a
flag-test-then-`WaitForSingleObject(INFINITE)` handshake with the render worker (`0xBB1950`:
`cmp [esi+8],0; jne skip; Wait(INFINITE)`). The doubled draw runs that handshake twice per tick;
a wakeup delivered between the second flush's flag test and its wait is lost forever. The wedged
stack reads `B8108F BB1963 69FD33 4EF4A6` - the exact chain, recovered live by the watchdog on
2026-08-02, with the render worker visible parked in its own wait on the other side.

Session 26 had concluded the Draw path has no submit handshake (the flush call hides behind a
link thunk and virtual dispatch), which is why 1t was never ported until session 35 refuted it.

## The fix

**`reentry 1t` - structural single-threading while stereo is armed, BS1's session-8 cure with
BS2's constants** (never shared, never in core): a hook on the flush point forces its
byte-confirmed INLINE branch (args into the render mgr, mode stamped single-threaded, drain
called on the game thread), with a drain guard skipping null-scene entries (the drain
dereferences `[mgr+0x24]` with no null check). The hw-thread quotient global is never poked -
its load-path consumers must see the true core count, which is why this survives load crossings.
`vrstereo on` arms it automatically (1t -> camera mode -> stereo, BS1's order).

Measured under 1t: `mode=1T`, `2nd/s == draws/s` (full-rate stereo, both eyes every frame),
`forced/s == 2x draws/s`, `wait2/s == 0` (the wait is structurally unreachable), presents on the
game thread, ~15% draw-rate cost (~91 -> ~78 draws/s; BS1 pays ~20% for the same cure).

## "Which change introduced it" - ANSWERED, and it was neither suspect

Session 36 instrumented the flush point (`wait2/s` on the beat line = second flushes that will
enter the INFINITE wait) and A/B'd resolution:

| run | resolution | wait2/s | wedge onset |
|---|---|---|---|
| baseline | 1920x1080 | == 2nd/s (91-94), set2 = 0 | ~5 s |
| A | 1280x720 | == 2nd/s (107-111), set2 = 0 | ~35 s |

The second flush entered the freeze-window wait on EVERY doubled frame at BOTH resolutions -
the race window has been open ~100x/s since the doubled draw landed at `97a229a`. The
resolution picker (`395893d`) and the viewmodel lens (`d425fab`) created no reachability;
`fgfov`/`vrfov` A/B runs were moot (wait2 already saturated). The 5-100 s onset is per-wait
lost-wakeup probability, so "it began after the resolution/FOV work" was onset-variance
coincidence. The 4-commit bisect is superseded by this measurement.

## How to reproduce the original wedge (for regression work only)

The doubled draw without 1t is dev-gated behind srdev; the freeze needs it opened by hand:

```powershell
.\tools\soak.ps1 -Game bs2 -Minutes 5 -Arm "reentry srdev on; vrstereo on; reentry 1t off" -KillOnFail
```

(`vrstereo on` arms 1t as part of its ladder; the trailing `1t off` drops it while stereo
stays armed - srdev is what lets that state exist. Expect exit 3 with `WATCHDOG secondDraw
stuck` and the `B8108F BB1963 69FD33 4EF4A6` stack in pacetrace.log. Load the save when the
title screen appears - tests run in the save, not the menu.) The healthy-run controls are
`vrcam on` and `vraer on`, which never re-enter the draw, and `vrstereo on` as shipped
(1t armed).

`tools/soak.ps1` is the acceptance instrument: exit 0 pass / 2 log stalled / 3 WATCHDOG /
4 process died / 5 no gameplay / 6 arm failed / 7 inconclusive / 8 new crash dump / 9 preflight
refused. `-Boot map -Map Ghetto.bsm` boots unattended into gameplay in ~2 min.

## Dead ends - do NOT retry (all measured, session 34)

1. **PulseEvent -> SetEvent redirect.** The engine never calls PulseEvent on this path
   (`PulseEvent calls 0` measured). Binary adjacency is not a calling relationship.
2. **Bounding the INFINITE wait** (IAT clamp, 2000 ms, scoped to the re-entered draw). The
   freeze became a CRASH (fault at 101E1A4B, repeated) - the engine proceeds to use a resource
   that genuinely is not ready. This did confirm the wait is the freeze point.
3. **A foreground gate on the doubled draw.** The freeze reproduces focused (`fg=1` in the
   trace); the alt-tab correlation was coincidence.
4. **Poking the hw-thread quotient (`0x149760C`) to force inline mode.** Never tried on BS2 and
   never will be: BS1's equivalent poke crashed a loader thread. Hook the flush point instead.

## The instruments that cracked it (all still in the tree)

- `pacetrace.log` - dedicated thread, own `_SH_DENYWR` handle, never takes `BVR_LOG`'s mutex,
  so it keeps writing while the game is wedged.
- The stall watchdog - suspends the wedged thread, walks its stack for game-image return
  addresses. Session 35 made it able to fail ANY run (it used to require an open draw stage,
  which only the doubled draw opens) and gave `watchdog_all_threads()` a voice (it showed the
  render worker's side of the deadlock).
- `wait2/s` / `set2/s` on the `[reentry] beat` line - latch state sampled at second-flush entry.
- `tools/disasm-rva.py` - offline disassembly; its output is game-derived and never committed.
