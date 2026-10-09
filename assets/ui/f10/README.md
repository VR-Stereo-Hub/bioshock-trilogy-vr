# F10 Rapture artwork

These six original PNGs were created for this menu during the 2026-10-08 design
work. They are not extracted game assets. They are the byte-identical assets
used in the approved native ImGui draft: underwater background, brass section
banner, bronze button, selected gold button, glass pipe and water fill.

`menu_resources.rc.in` embeds them in the DLL and in the offscreen preview.
Windows Imaging Component decodes the embedded resources at UI initialization.
The renderer uses alpha bounds and three-slice caps without modifying the files.
No artwork has to be installed separately. Fonts are loaded from Windows, with
an ImGui fallback; font files are not redistributed.
