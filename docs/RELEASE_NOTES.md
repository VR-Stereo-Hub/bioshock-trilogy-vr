# Release notes

Newest first. The version is read from `CMakeLists.txt` by `tools/package.ps1`, so the zip
name, the DLL banner and the tag cannot disagree.

## v0.5.0 - cutscenes, and an aim dot you can trust

**Cutscenes are the headline.** Session 28 changed the projection claim, the foreground lens
and the frame pacing, and cutscenes are the one place all three meet - so this release exists
to make that intersection right rather than to add features around it.

### The wrench hits again

Melee had been unreliable in a way that looked random and was not: the engine's own idea of where
you were aiming had frozen pointing at the floor, and swings went there. Worst when you looked
down, which is why fights and the rocks at the start were the bad cases, and why a wall you walked
up to squarely still worked. Guns were never affected.

The chase for this went through two wrong answers first, both eliminated by measurement rather
than argument, and both are recorded so nobody repeats them: the aim substitution (melee never
reaches it) and pad lock-on (that setting was never taking effect at all).

### The health and EVE bars have their colour back

A change in this release cycle was sending the bars' colour fills into the world instead of onto
the HUD panel, so the bars read as empty. Same fix reverts full-screen effects to the HUD panel,
which is where they were in v0.4.1 - see the known issue below.

### Known issue: full-screen effects sit on the HUD panel

Water and damage effects ride the floating HUD panel rather than covering your view. Routing them
into the world was tried and is what emptied the health bars, and it could not have worked anyway:
the game authors them at HUD size, so moving them into the frame just puts a HUD-sized rectangle in
the middle of your view. Covering the view needs different geometry, which is the next piece of
work rather than a switch.

### Known issue: cutscenes sit low with black borders

Ticking **Game FOV write** in the F10 overlay makes them fill the view, but it is not recommended
for normal play yet.

### Cutscene black bars are gone

The bars were never part of the picture. They are a flash sprite (`WidescreenBars`) that the
game draws **over** a full-frame image, so skipping that one draw reveals what was always
underneath - nothing is cropped, stretched or re-projected. Off by default? No: **on** by
default, because there is no trade. "Hide cutscene black bars" turns it off if you want them.

This corrects a long-standing wrong model. Earlier builds assumed the engine squeezed the
picture into a middle band and tried to stretch it back; that was a real risk of cropping
picture, and it never worked. The stretch is deleted.

### The rig behaves during a cutscene

New "During cutscenes" dropdown:

- **authored** (default) - the director's camera and the authored hand animation play exactly
  as they do in the flat game.
- **authored + head look** - the choreography plays, but you can look around. Your head adds
  to the shot rather than replacing it, so the scene still opens framed as intended.
- **off** - your head and hands drive straight through the scene, as in earlier builds.

Previously the controllable hands could override the authored ones, and a hidden hand could
stay hidden for the rest of the scene. Both are fixed: the drives are suspended explicitly and
the viewmodel skeleton is handed back to the engine.

### Cutscene subtitles are readable again

They now ride the head-locked panel - one image in both eyes. Rendered into the world they were
captured separately per eye, from different game frames, and the text doubled. "Cutscene
subtitles in-frame" restores the old behaviour if you want it.

### Aim dot

A single dot on the ray the bullet actually uses. It is not a reconstruction that happens to
agree - it is built from the exact values handed to the game's fire code, so where the dot sits
is where the shot goes. Off by default; "Aim dot" turns it on.

Set **aim dot distance** to roughly your calibration wall's distance before tuning. A dot and a
bullet hole only line up in stereo when they sit at the same depth, otherwise you will chase a
mis-registration that is not a trim error.

### Also

- The dot only appears while the mod is genuinely steering your shots, so it doubles as a
  live indicator that aim substitution is working.
- Cutscene detection no longer depends on reading black pixels off the frame, which never
  worked with a headset attached - it keys on the bar draw itself.

### Upgrading

Nothing to do. Your own tuning in `%LOCALAPPDATA%\BioshockVR\` still wins key by key, and all
of the above are new keys with sensible defaults, so an existing install picks them up without
touching anything you calibrated.
