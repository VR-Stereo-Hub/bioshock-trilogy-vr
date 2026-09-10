# Known issues and review findings

Living list. Each entry says which game it touches, where it came from, and its
state. Fixed entries stay for one release so a tester log can be matched to the
build that closed them; then prune.

## BioShock 2

### Left-eye flicker (issue #31) - FIXED in s74, headset-confirmed

The hand/weapon "snap" in the left eye was the engine's dirty-flagged skeleton
update re-evaluating the hands inside pass 1 (never pass 2), after every repaint
site and before the mesh was drawn. Fixed by hooking that update and repainting
the driven pose the moment it returns (`vrbones wfix`, ships ON, F10 checkbox
"LEFT-EYE FLICKER FIX (s74)" is the in-headset A/B). Mechanism, evidence and
derivation: `docs/bioshock2/ENGINE_NOTES.md` session 74.

Review findings on the fix, all closed in the same PR (s74 code review):

| # | Finding | State |
|---|---|---|
| 1 | Hook thread id latched once at install; 0 if the rig resolved before the first Draw (fix silently inert, still reported hooked) | fixed: refreshed at every Draw entry |
| 2 | Saved-return slot never self-healed after a swallowed exception (fix silently dead for the session) | fixed: cleared at Draw entry and on world change |
| 3 | Hook installed from inside a hooked Draw / from the F10 render thread; one enable failure stuck forever | fixed: rig resolve and the checkbox POST; the game thread's poll lane installs; ALREADY_CREATED retries the enable |
| 4 | Six-point race probe ran unconditionally (~18 syscalls per stereo pair) | fixed: off unless `vrbones race on` / `diag31 on` |
| 5 | `pe_repaint` paid a VirtualQuery before the 48-byte sentinel compare on every ProcessEvent | fixed: compare first, validate on a hit |
| 6 | `vrbones status` invariant always read BROKEN (new repaint phases not summed) | fixed: all 8 phases summed; phase 4-7 window max drained |
| 7 | Engine numbers (update slot 0xA4, call site) lived in `bones.cpp`; log printed the call site off by one | fixed: `patterns.h` (`kSkelInstUpdateSlot`, `kSkelInstDirtyOffset`, `kSkelInstUpdateCallSiteRva`) |
| 8 | `[hudgate]` logged a false burn on re-arm and flooded with stereo off; `[pairEdge] unheld-right` flooded with pair pacing off | fixed: unconditional delta tracking, eye-tagged presents only, transition latch |

### Weapon scale flicker (big/small) - OPEN, not reproduced

Reported once on the flat window with the hand drive off (s74). A 74-frame
weapon-region sweep on a fresh boot was flat in both eyes. Suspects: foreground
FOV write cadence, weapon-profile / `wscale` writes, or long-uptime state.
Needs a long soak with `hash every 1` + the weapon-region metric.

### Game FOV option baked by a hard kill - OPEN

While the mod's game-FOV write is live, a crash or power loss leaves the written
value in `Shared.ini` as if it were the user's option (seen twice in s74:
`saved option 138`, real option 100). The restore only runs on a clean exit.
Candidate fix: write the restore value to a sidecar at write time and repair
on the next boot.

### Menu-transition HUD burn - REAL BUT NOT PERCEIVED, parked

Every pause open/close draws the HUD into both eyes' world images for ~3 pairs
(`[hudgate]` witnesses it 5/5). The user does not perceive it in the headset,
so it is parked; the instrument stays.

## Cross-game (core)

### F10 panel swallow of the right trigger and stick - FIXED in s74 for BS2

The s63 controller-driven panel zeroed RT and the right stick for the game while
the overlay was visible. On BS2 the panel cannot be closed from inside the
headset (no F10 key), so one F10 visit killed fire and turning for the session.
Now gated to pad-drive mode (BS1 opt-in), the only mode where the swallow has a
purpose; the overlay logs SHOWN/hidden.

Left as-is, noted:
- BS1, pad drive on, right hand untracked, panel open via keyboard: RT and the
  right stick are still swallowed although nothing can click. Pre-existing s63
  behaviour; a `wants_pad_input()` predicate on the overlay would be the right
  home if it ever matters.
- With the panel open on the desktop, a BS2 wrench-swing pulse can still fire
  (the swing pulse was only ever swallowed by the same gate). Design question
  for the controller-navigation work, not a regression anyone hit.

## Simulator and tools (diagnostic-only)

- `hash every 1` copies each eye's full source texture and Maps it synchronously
  on the present thread every frame - a GPU drain that perturbs the pacing it
  measures. Nothing in s74 rests on it (the fix was A-B-A'd by counters and
  captures), but treat hash-mode timing numbers as approximate. Cheaper form if
  it ever matters: double-buffered staging (Map the previous frame) or a GPU
  downsample.
- `tools/eye-seq-diff.ps1` builds its line array with `+=` (quadratic); fine for
  minutes, slow for a multi-hour TSV. Stream the rows if a long soak needs it.
- Cleanup deferred to the healing session: `module_range()` in `bones.cpp`
  duplicates the image-range lookup that `capture_main_module` already returns;
  ad-hoc `eip - g_gameBase` RVA math could share a range-checked `to_rva()`;
  the two staging-copy paths in `xrsim_compositor.cpp` could share a helper.

## Repository hygiene (s74)

- Three s74 commit subjects run 73-75 characters (limit 72); the fix commit
  bundles the diagnosis levers with the fix. Left in history (rewriting a
  pushed draft branch was not worth it); split and shorten on the next one.
