# The hand pivot: a measurable test, and the loop until it is gone

Written at the end of session 86 (2026-10-08) for the next session, which runs WITHOUT a
headset: the tester is in bed, and has said so: **the simulator is the test, and launching
it is authorised for this question.** Not the real game through Steam, not the headset.

## The report

Through s83-s86f every build has had the same complaint in some form: the hand "pivots"
instead of being attached to the controller. The last run (s86f, palm-centre anchor, step
rows): "hand forward and normal it was synced, but pointing it in different directions
causes huge desync between the controller and the hand position." s83's run, which used
mode 3's actor placement, had no such report.

## What is known for certain

- `hand_compose::delta` pins the palm it is given and turns the hand about it; the
  hand-compose sweep proves the written palm stays on its target to 2e-5 UU through a
  180-degree wrist swing (pure, offline). So the WRITTEN skeleton is right, for the point
  it is told to pin.
- Whether the RENDERED hand is where the written skeleton says it is has never been
  measured. Three things sit between the write and the pixels and none is verified:
  1. the actor's DrawScale (0.80) - the s86 size runs show it scales positions AND skin,
     and the compose divides positions by it; if the renderer's scale is not exactly what
     `ds_read` returns, every written point lands off by that ratio TIMES its distance from
     the actor origin (the camera), which grows as the hand moves away from the centre of
     view: exactly "synced forward, desync off to the side";
  2. the actor transform read at compose time vs the one the renderer uses (the engine
     re-places the actor on the camera every tick; the late compose re-reads it, but the
     renderer may use a later one);
  3. the foreground scene's lens (`FOVPROBE`: the mod writes ForegroundFov to match the
     world lens; if the foreground projection and the world projection differ, a hand at
     the right world point renders at the wrong screen angle, and the error grows with the
     angle off the view centre - also "synced forward, desync to the side").
- Dishonored has none of these: it composes D onto the bone palette of the very draw that
  renders the hand, in that draw's own space, read from its shader constants. Whatever its
  engine does to the component or the camera, the hand lands where the pixels say.

## The oracle

`vrhands pivotprobe on` (s86g) prints, 5 Hz per hand, after the late compose:

```
[bones] PIVOTPROBE R: palm (written, read back, to world) x y z | controller grip x y z
       | target x y z | error E cm (fwd f right r up u) | hand A deg off the view
       | actor loc ... rot ... DrawScale k | trim pos ... rot ...
```

`palm` is the mean of the five palm bones READ BACK from the engine's array after the
write, taken to world through the actor transform and DrawScale the renderer is about to
use. `controller grip` is the raw grip point. `error` is the distance between them.

Two levels, two verdicts:

| | means | next |
|---|---|---|
| `error` stays under 1 cm at every orientation, and the captures show the hand on the laser's start at every orientation | there is no pivot in this build; the report was something else (retune the trim with the step rows, or a wrong weapon grip) | headset |
| `error` stays under 1 cm but the CAPTURES show the hand off the laser by an amount that grows with `hand A deg off the view` | the skeleton is right and the render is wrong: DrawScale, actor timing or the foreground lens (items 1-3 above). Test each with its lever: `vrfgfov off`, a build that does not divide by `k`, the late compose reading the actor one frame later | fix, re-run the sweep |
| `error` grows with orientation | the compose itself: the anchor, the actor transform read, the DrawScale division. The line prints all three | fix, re-run the sweep |
| no PIVOTPROBE lines | the drive is not running (not in gameplay, hands not tracked, or the probe not armed) | boot again |

## The sweep (`tools\pivot-sweep.ps1`)

```powershell
.\tools\xrsim-selftest.ps1                       # the sim is healthy
.\tools\pivot-sweep.ps1 -Launch                  # xrsim-launch, boot -Attach, then the sweep
.\tools\pivot-sweep.ps1 -Hand l                  # the left hand, game already up
```

It holds the right grip at one point (0.20, 1.20, -0.40 m; head level, forward) and turns
it through forward, yaw +-30/+-60, pitch +-45, roll +-90 and two diagonals, reading the
probe and taking a per-eye capture at each. It prints a table (error per orientation, in
body cm: forward / right / up) and writes `pivot-sweep.json` and the captures under
`%TEMP%\bvr\pivot`. Exit 0 under 1 cm everywhere, 1 otherwise, 2 if no probe line came.
**It has not been run yet** - it was written blind at the end of s86; expect to fix its
regexes or settle times on the first run.

For the render-level check turn the laser on first (`.\tools\game-cmd.ps1 "vrlaser on"`;
vrpreset has `laserOn=0`) so each capture has the controller's ray as a reference, and
read the capture PNGs: the hand's palm should sit at the ray's start in every capture.

## The loop

Until the sweep passes at BOTH levels:

1. `.\tools\pivot-sweep.ps1 -Launch` (or without -Launch while the game is up).
2. Read the table and the captures. Decide which row of the oracle applies.
3. Change ONE thing, with its reason in the code. Build. `.\tools\install.ps1 -Game bs1`.
   The sim needs the game restarted for a new DLL: close it (`Stop-Process -Name
   BioshockHD`), then `-Launch` again.
4. Record each iteration in HANDS_DISHONORED.md (one line: what changed, worst error,
   verdict) so the next session does not repeat it.
5. Stop when: worst error < 1 cm at every orientation AND the captures show the hand on
   the ray at every orientation. Then STATUS.md, and the headset question for the tester.

If after the render-level levers (DrawScale, actor timing, foreground lens) the captures
still disagree with the probe, the bone-write path is not trustworthy on this engine and
the next step is **Dishonored's palette system** - see DISHONORED_PIPELINE.md 1.2-1.4 and
the plan below.

## The palette route (the 1:1 fallback)

What it is: hook the D3D11 draw of the hands mesh, read the bone palette the engine put in
the skinning constant buffer for THAT draw, compose D onto the hand bones' matrices in the
draw's own space, write the buffer back, let the draw run. The target is built from the
controller pose through the draw's own view and local-to-world constants, exactly as
Dishonored's `MpAcquireCtx` + `MpWorldTarget` + `MpBuild` do.

What must be found first (static, IDA, then the simulator), none of it known today:

- which draw(s) render the hands mesh (the foreground scene; `scenedraw.cpp` already
  finds the foreground pass; a draw census like Dishonored's `draw_census.cpp` finds the
  call by vertex count and stride);
- the skinning constant buffer: slot, matrix count (47 bones), layout (3x4 or 4x4, row or
  column major, component or world space, or dual quaternions);
- the view and local-to-world constants of that draw, to build the target in its space;
- the hand's dominant (wrist) slot and the palm anchor in draw space.

What it buys: the pixels cannot disagree with the target, by construction. What it costs:
an engine-side investigation of the renderer that this repo has not done, and the arm IK
would move to the palette too (Dishonored's `arm_ik_draw.inc`).

Do the measurement first. If the probe says the skeleton is right and the render is wrong
in a way the three levers do not fix, the palette route is justified and this doc is its
brief.

## Result (s87, 2026-10-08/09)

Both levels pass, on the bone drive and on the palette route that replaced it; the iteration
log is HANDS_DISHONORED.md s87. The pivot was the stored grip trims (wrist-era values applied
in the palm frame), not the render: 9.49 / 8.49 cm constant, 0.00 once zeroed.

How the loop runs now:

```powershell
Stop-Process -Name BioshockHD; .\tools\install.ps1 -Game bs1
.\tools\xrsim-launch.ps1 -Game bs1
.\tools\sim-load-save.ps1 -Row 10          # the Aug 03 11:01:57 PM save, through the menu
.\tools\pivot-sweep.ps1 [-Hand l -Point "-0.10 1.45 -0.45"] [-Out <dir>]
```

- **Never Continue** (the tester's rule). `boot.ps1 -Attach` presses A through the menu and so
  presses Continue; `sim-load-save.ps1` loads the row asked for and waits for each press to be
  consumed.
- **One `game-cmd` write per batch.** It replaces `command.txt`, which the game reads from
  CalcView about once a second; two calls in a row lose the first (that is why the first
  palette sweep read no probe lines).
- The default grip point is now in view (0.10 1.45 -0.45, about 18 deg down); the first one sat
  45 deg down with the hand half off the frame.
- The laser is not a usable render reference in the simulator (row 3 of the log). The render
  check is the hand and the held weapon holding one screen point while the controller turns.

