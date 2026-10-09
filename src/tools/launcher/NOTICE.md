# Launcher provenance

This is an adapted version of the Dishonored VR launcher's native Windows
architecture, built for BioShock Remastered. It is not the original launcher.

The `sys/fs`, `sys/process`, `sys/gpu`, `sys/game_ini` and `sys/updates` files
derive from `VR-Stereo-Hub/Dishonored-VR/src/tools/installer`, inspected on
2026-10-08. The original license is retained in `DISHONORED-LICENSE.txt`.
The Win32/D3D11 window and offscreen rendering approach also follow that launcher.
BioShock's installation plan, settings mapping and interface are adapted here.

The six Rapture textures and theme renderer come directly from the BioShock
F10 menu in this repository. No game assets were extracted for this launcher.
Icon candidates are original images created with the built-in image generation
tool. They are review candidates until the owner chooses one.
