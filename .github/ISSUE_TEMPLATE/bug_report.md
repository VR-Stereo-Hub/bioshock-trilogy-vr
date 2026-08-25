---
name: Bug report
about: Something in the mod misbehaves
labels: bug
---

**What happened, and what you expected instead**

**Which game** (BioShock Remastered / BioShock 2 Remastered / BioShock Infinite)

**Headset and OpenXR runtime** (Quest 3 + Virtual Desktop/VDXR, Steam Link/SteamVR, other)

**Mod version** (from the top of the log, or the release zip name)

**Steps** - what you were doing when it happened

**Log** - attach `%LOCALAPPDATA%\BioshockVR\bioshockvr.log` from the run that showed the
problem (BS2: `...\BioshockVR\bs2\`, Infinite: `...\BioshockVR\bsi\`). Without it almost
nothing can be acted on. If the game crashed, attach the dump from
`%LOCALAPPDATA%\BioshockVR\crash\` too.

**Did you try a clean settings folder first?** Close the game, move `vrpreset.ini`,
`hands.ini`, `weapons.ini` and `command.txt` out of `%LOCALAPPDATA%\BioshockVR\`, relaunch,
press VR PRESET 1. This fixes most odd behaviour and it tells us a lot either way.
