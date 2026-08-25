## What

<!-- One sentence a player would understand. -->

## Why

<!-- The measurement, the field report or the defect that prompted this. -->

## How it was verified

<!-- Be precise. Which xrsim script, or which headset run and what you looked at.
     "Tested" on its own is not a verification claim. -->

- [ ] Builds `Release | Win32`
- [ ] Simulator (`tools/xrsim-*.ps1`) - which:
- [ ] Headset - which game, which runtime, what was checked:

## Not verified

<!-- Every PR has some. Saying so is what lets the reviewer aim their attention. -->

## Risk

- [ ] Touches a hook, a pattern scan or a memory write (say where, and how it fails closed)
- [ ] Changes a BS1 code path (BS1 is the headset-accepted baseline)
- [ ] Changes shared `src/core/` (is it purely additive?)
- [ ] Adds or moves an engine address (documented in the game's `ENGINE_NOTES.md`?)
