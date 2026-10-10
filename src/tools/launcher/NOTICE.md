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
The initial icon candidates were created with the built-in image generation
tool. The current medallion is an AI-assisted adaptation of the owner's supplied
BioShock logo references, with VR added beneath the wordmark. It is not an
original BioShock trademark; the BioShock name and artwork belong to their
respective owners. Reference provenance and prompts are in
`assets/ui/launcher/README.md`.
