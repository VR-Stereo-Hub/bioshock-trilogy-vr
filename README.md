# bioshock-vr

A native VR mod for **BioShock Remastered** (PC, Steam): stereoscopic rendering, 6DOF head
tracking, and motion controllers - weapons in one hand, plasmids in the other - targeting
Quest 3 via Virtual Desktop (VDXR/OpenXR) or Steam Link (SteamVR), and any other OpenXR headset.

The mod is a DLL injected into the game's process. It hooks the game's DirectX 11 renderer and
the Vengeance engine (Unreal Engine 2.5 lineage) camera path, and drives them from an OpenXR
session. No game files are modified and no game assets are distributed.

> **Status:** playable. Working today: full-rate stereo rendering, 6DOF head tracking,
> motion-controller aim with a laser (right hand = weapons, left hand = plasmids), the visible
> viewmodel following the controller with the inactive hand hidden, **per-weapon aim profiles**
> (every weapon keeps its own laser calibration and swaps it in the moment you equip it),
> body-follows-head movement ("walk where you look"), the game HUD (health/EVE/ammo - and the
> pause menu) on a readable floating panel in VR, VR-standard controller bindings with
> ammo-select on the right stick, a single-eye desktop mirror, and in-headset tuning sliders
> that persist. The shipped defaults ARE a full calibration (aim trims, per-weapon profiles,
> body follow) tuned in-headset on a Quest 3. See [docs/STATUS.md](docs/STATUS.md) for the
> current state and [docs/ROADMAP.md](docs/ROADMAP.md) for what is next.

## Requirements

- BioShock Remastered on Steam (`steamapps\common\BioShock Remastered`)
- A PCVR-capable headset. Primary target: Meta Quest 3 with Virtual Desktop (VDXR) or Steam
  Link (SteamVR); any OpenXR runtime with a 32-bit loader should work

## Install (release zip)

1. Download the release zip and copy **both DLLs** (`xinput1_3.dll`, `bioshockvr.dll`) into the
   game's binary folder:
   `...\steamapps\common\BioShock Remastered\Build\Final\`
2. If you use **itsloopyo's head-tracking mod**, remove or back up its `xinput1_3.dll` first -
   the two mods use the same injection vector and cannot coexist.
3. Headset side (Quest 3 + Virtual Desktop): in Virtual Desktop's Streaming tab set the OpenXR
   runtime to **VDXR**, connect, then launch the game from Steam inside Virtual Desktop.
   (Steam Link / SteamVR works too - the mod talks to whatever OpenXR runtime is active.)
   **Set the game's resolution to roughly SQUARE, not 16:9** - something like 2700x2700.
   The mod sizes the eye render target from the game's backbuffer, and headset panels are
   near square, so a 16:9 backbuffer renders a wide strip that the headset then throws
   away. At 3840x2160 on a Quest 2 only ~54% of the width is inside the FOV: a square
   2750x2850 has *fewer* total pixels, is sharper in the headset, and runs faster. The
   startup log prints the consequence - `xr: headset fov half-angles ... -> game hfov N deg
   (aspect A)`; the closer `aspect` is to 1.0, the less you are wasting.
4. Launch the game through Steam. The mod logs to `%LOCALAPPDATA%\BioshockVR\bioshockvr.log`.

To uninstall, delete the two DLLs (restore itsloopyo's backup if you made one).

## Playing in VR

1. Load into the game flat first (menus are still flat-screen for now).
2. Press **F10** to open the mod overlay and click **VR PRESET 1** - one press arms
   everything in the right order: VR pacing, 6DOF camera, motion controllers, controller aim +
   laser, the viewmodel drive, body-follows-head, and stereo last. No restart is ever
   needed - the mod answers the game's one-shot startup gamepad check itself, so the
   motion controllers engage the moment the preset is pressed, first launch included.
3. Quest 3 Touch controls:

| Input | Action |
|---|---|
| Right trigger | fire weapon (first pull raises it) |
| Left trigger | cast plasmid (first pull raises it) |
| Right grip | switch/cycle weapon (hold for the radial) |
| Left grip | switch/cycle plasmid (hold for the radial) |
| Left stick | move (crouch on click) |
| Right stick | turn; **hold the LEFT thumbrest + push up/down/left = select ammo type** (zoom is removed in VR) |
| Left thumbrest | ammo-select modifier (the pad above the X/Y buttons - just rest your thumb on it) |
| A | use / interact (and menu confirm) |
| B | jump |
| X | reload / hack / inject EVE |
| Y | first-aid kit |
| Left menu button | pause (hold: map/objectives) |

   Under VR the right stick no longer pitches the view (your head does); `vrinput pitchkill
   off` restores stick pitch if you want it back.

   **Ammo select** used to mean holding the right stick *click* while pushing that same
   stick, which is awkward. It is now the **left thumbrest** - the smooth pad above the
   X/Y buttons, which senses your thumb resting on it. It has to be the left one: your
   right thumb cannot rest on the right pad and push the right stick at the same time.
   The overlay's "Ammo-select modifier" combo (or `vrinput ammomod click|thumbrest|both`)
   switches back to the stick click, or accepts either. Controllers with no thumbrest
   (Pico, some SteamVR setups) keep the stick click automatically.

Tuning (all in the overlay, all persisted by **"Save preset values"** / `vrpreset save`):

- **World scale / IPD / game FOV** - comfort and scale calibration
- **Per-hand aim trim** (up to +-90 deg) - laser/bullet direction alignment per hand
- **Per-weapon profiles** - the right hand's aim trim + ray offsets automatically follow the
  EQUIPPED weapon: tune with a weapon up and only that weapon's profile changes, swapped in
  the moment you switch. Calibration flow: fire at a wall, nudge the sliders until the laser
  sits on the bullet holes, next weapon, then one "Save preset values". The overlay's
  "weapon profile:" line shows which weapon you are editing.
- **Per-hand ray offsets** - the "Ray offset hand: L / R" selector + three sliders move the
  laser (and the bullets with it - they are one ray) to line up with the controller and model
- **Head anchor offsets** - if the camera sits wrong in the body
- **Per-hand model offsets** - the "Tuning hand: L / R" selector picks which hand the six
  position/rotation sliders edit, so the pistol and the plasmid hand are tuned independently
- **Turn controls** - "Smooth turn speed" scales the stick turn rate; "Snap turn" replaces
  smooth turning with discrete steps (angle slider, default 45 deg)
- **Cinematics** - scripted scenes (the bathysphere descent), the hack minigame, loading
  screens and FMVs are auto-detected. Cutscenes play as a full stereo projection with
  head-look by default; untick "Cinematics as stereo projection" to watch them on a big
  virtual screen instead. Flat 2D screens (hacking, loading) always use the readable screen.
- **Cutscene black bars are gone** ("Hide cutscene black bars", on by default). The game's
  widescreen bars are a flash sprite drawn over the full picture, so hiding them reveals the
  image that was always underneath - nothing is cropped, stretched or lost.
- **What the rig does during a cutscene** - the "During cutscenes" dropdown:
  *authored* (default) plays the director's camera and the authored hand animation exactly as
  the flat game does; *authored + head look* keeps the choreography but lets you look around;
  *off* leaves your head and hands driving straight through the scene.
- **Cutscene subtitles** ride the head-locked panel so they stay readable in stereo. The
  "Cutscene subtitles in-frame" checkbox puts them back in the world if you prefer.
- **Aim dot** ("Aim dot", off by default) - a single dot on the ray the bullet actually uses,
  not a reconstruction of it, so where the dot sits is where the shot goes. Set "aim dot
  distance" to roughly your calibration wall's distance before tuning: a dot and a bullet hole
  only line up in stereo when they are at the same depth.
- **`vrbody off`** - live A/B for the body-follows-head transfer (instant 1:1 by default)

### The bundled preset (v0.3.0+)

**A fresh install needs no tuning**: the shipped defaults are a complete in-headset
calibration (left/plasmid hand trim, per-weapon profiles for all eight holdables, body
follow, HUD placement). Just install and press VR PRESET 1.

The release zip also carries the same calibration as plain files (`vrpreset.ini`,
`hands.ini`, `weapons.ini`):

- **New users**: nothing to do - the DLL defaults are identical to these files.
- **Existing users with their own tuning**: your files in `%LOCALAPPDATA%\BioshockVR\`
  ALWAYS win over the built-in defaults, key by key - updating the DLLs changes nothing you
  tuned. To adopt the bundled calibration instead, back up and delete (or overwrite) those
  three files in `%LOCALAPPDATA%\BioshockVR\` and restart the game. To adopt only parts
  (say, the weapon profiles but not your world scale), copy just that one file - or even
  single lines: every `key=value` line stands alone.

The flat-screen crosshair is hidden by default (the laser replaces it); the "Flat-screen
crosshair" checkbox or `vrxhair on` brings it back.

The game HUD (health, EVE, ammo - and the pause menu) shows on a head-locked floating panel
during stereo gameplay; "HUD distance/width/height offset" sliders place it, `vrhud off`
disables the capture entirely, and the inactive hand's model is hidden while the other hand
is raised (`vrhands hideinactive off` shows both).

The desktop window mirrors the **left eye** while stereo runs (`vrmirror off` restores the raw
alternating view), and the game keeps running at full speed on the monitor when you take the
headset off (`vrpace off` restores the old blocking behavior).

## Build from source

```powershell
git clone --recursive https://github.com/mohamad-balouza/bioshock-vr
cd bioshock-vr
.\tools\build.ps1            # Debug build (finds the VS-bundled CMake automatically)
.\tools\build.ps1 -Release   # Release build
.\tools\install.ps1          # copies the mod DLLs into the game's Build\Final folder
.\tools\uninstall.ps1        # removes them (restores anything it backed up)
```

Building needs Visual Studio 2022 Build Tools with the **x86** MSVC toolset (the game is
32-bit) and git. `.\tools\tail-log.ps1` follows the log live.

## Legal

This project is not affiliated with, endorsed by, or connected to 2K Games, Take-Two
Interactive, or any of their subsidiaries. It distributes no game assets, no decompiled game
code, and no copyrighted material - only original injection code. A legitimately owned copy of
BioShock Remastered is required. Free and open source, forever.

## Credits

- [itsloopyo/bioshock-remastered-headtracking](https://github.com/itsloopyo/bioshock-remastered-headtracking)
  (MIT) - pioneered the `xinput1_3.dll` injection vector and the `PlayerCalcView` camera hook
  technique on this exact game; this project ports and extends those techniques.
- [praydog/REFramework](https://github.com/praydog/REFramework) (MIT) - reference implementation
  for OpenXR/D3D11 VR integration in a closed-source engine.
- **[BioVRDev/Bioshock-Remastered-VR](https://github.com/BioVRDev/Bioshock-Remastered-VR)** - a
  parallel native VR mod for the same game, and a genuinely friendly one. The two projects have
  swapped findings in both directions: their README credits this one for the
  reticle-via-console-Exec technique, the arm bone indices and the render-target HUD capture,
  and their author has given explicit permission to reuse their code and concepts here. Ideas
  taken from them so far: rendering at a near-square resolution matched to the headset panel
  instead of widening the game's FOV, the "report the game's own symmetric FOV to the
  compositor, never the headset's canted one" invariant, per-feature build guarding that logs
  and stands down instead of trusting an address, and the startup config echo block. Files that
  adapt their code carry an attribution comment naming the source. Worth a look, and worth
  trying if this mod does not suit your setup.
- Third-party libraries: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

[MIT](LICENSE)
