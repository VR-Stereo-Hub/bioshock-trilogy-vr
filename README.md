# bioshock-vr

A native VR mod for **BioShock Remastered** (PC, Steam): stereoscopic rendering, 6DOF head
tracking, and motion controllers - weapons in one hand, plasmids in the other - targeting
Quest 3 via Virtual Desktop (VDXR/OpenXR) or Steam Link (SteamVR), and any other OpenXR headset.

The mod is a DLL injected into the game's process. It hooks the game's DirectX 11 renderer and
the Vengeance engine (Unreal Engine 2.5 lineage) camera path, and drives them from an OpenXR
session. No game files are modified and no game assets are distributed.

> **Status:** early development - project scaffolding and injection skeleton.
> See [docs/STATUS.md](docs/STATUS.md) for the current state and [docs/ROADMAP.md](docs/ROADMAP.md)
> for the milestone plan (stereo rendering, motion controls, hands, selection wheels, VR menus,
> BioShock 2 support).

## Requirements

- BioShock Remastered on Steam (`steamapps\common\BioShock Remastered`)
- A PCVR-capable headset. Primary target: Meta Quest 3 with Virtual Desktop (VDXR) or Steam Link (SteamVR)
- To build: Visual Studio 2022 Build Tools with the **x86** MSVC toolset (the game is 32-bit), git

## Build

```powershell
git clone --recursive https://github.com/mohamad-balouza/bioshock-vr
cd bioshock-vr
.\tools\build.ps1            # Debug build (finds the VS-bundled CMake automatically)
.\tools\build.ps1 -Release   # Release build
```

## Install / uninstall

```powershell
.\tools\install.ps1          # copies the mod DLLs into the game's Build\Final folder
.\tools\uninstall.ps1        # removes them (restores anything it backed up)
```

Then launch the game normally through Steam. The mod writes a log to
`%LOCALAPPDATA%\BioshockVR\bioshockvr.log` (`.\tools\tail-log.ps1` follows it live).

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
