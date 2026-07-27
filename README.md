# bioshock-vr

A native VR mod for **BioShock Remastered** (PC, Steam): stereoscopic rendering, 6DOF head
tracking, and motion controllers - weapons in one hand, plasmids in the other - targeting
Quest 3 via Virtual Desktop (VDXR/OpenXR) or Steam Link (SteamVR), and any other OpenXR headset.

The mod is a DLL injected into the game's process. It hooks the game's DirectX 11 renderer and
the Vengeance engine (Unreal Engine 2.5 lineage) camera path, and drives them from an OpenXR
session. No game files are modified and no game assets are distributed.

> **Status:** playable. Working today: full-rate stereo rendering, 6DOF head tracking,
> motion-controller aim with a laser (right hand = weapons, left hand = plasmids), the visible
> viewmodel following the controller with the inactive hand hidden, body-follows-head movement
> ("walk where you look"), the game HUD (health/EVE/ammo - and the pause menu) on a readable
> floating panel in VR, VR-standard controller bindings with stick-flick ammo switching, a
> single-eye desktop mirror, and in-headset tuning sliders that persist. See
> [docs/STATUS.md](docs/STATUS.md) for the current state and [docs/ROADMAP.md](docs/ROADMAP.md)
> for what is next (per-weapon aim alignment is the known big gap).

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
4. Launch the game through Steam. The mod logs to `%LOCALAPPDATA%\BioshockVR\bioshockvr.log`.

To uninstall, delete the two DLLs (restore itsloopyo's backup if you made one).

## Playing in VR

1. Load into the game flat first (menus are still flat-screen for now).
2. Press **F10** to open the mod overlay and click **VR PRESET 1** - one press arms
   everything in the right order: VR pacing, 6DOF camera, motion controllers, controller aim +
   laser, the viewmodel drive, body-follows-head, and stereo last.
   **First-ever launch only:** the game checks for a gamepad once at startup, so on a fresh
   install the motion controllers stay dead until you **restart the game once** after the
   first preset press (the press itself enables them for every later launch). If you had any
   physical controller plugged in when the game started, this does not apply.
3. Quest 3 Touch controls:

| Input | Action |
|---|---|
| Right trigger | fire weapon (first pull raises it) |
| Left trigger | cast plasmid (first pull raises it) |
| Right grip | switch/cycle weapon (hold for the radial) |
| Left grip | switch/cycle plasmid (hold for the radial) |
| Left stick | move (crouch on click) |
| Right stick | turn; **hold click + push up/down/left = select ammo type** (zoom is removed in VR) |
| A | use / interact (and menu confirm) |
| B | jump |
| X | reload / hack / inject EVE |
| Y | first-aid kit |
| Left menu button | pause (hold: map/objectives) |

   Under VR the right stick no longer pitches the view (your head does); `vrinput pitchkill
   off` restores stick pitch if you want it back.

Tuning (all in the overlay, all persisted by **"Save preset values"** / `vrpreset save`):

- **World scale / IPD / game FOV** - comfort and scale calibration
- **Head anchor offsets** - if the camera sits wrong in the body
- **Per-hand aim trim** - laser/bullet direction alignment per hand
- **Per-hand ray offsets** - the "Ray offset hand: L / R" selector + three sliders move the
  laser (and the bullets with it - they are one ray) to line up with the controller and model
- **Per-hand model offsets** - the "Tuning hand: L / R" selector picks which hand the six
  position/rotation sliders edit, so the pistol and the plasmid hand are tuned independently
- **`vrbody off`** - live A/B for the body-follows-head transfer (deadzone defaults 23 deg)

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
- Third-party libraries: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

[MIT](LICENSE)
