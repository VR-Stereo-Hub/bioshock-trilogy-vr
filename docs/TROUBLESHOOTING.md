# Troubleshooting

The fixes below cover every "no VR / flat mode" report received so far. Work through them
in order - the first two sections resolve the vast majority of cases.

## First: find your log

Every diagnosis starts with the mod's log file. Paste the folder path into the Explorer
address bar:

| Game | Folder |
|---|---|
| BioShock Remastered | `%LOCALAPPDATA%\BioshockVR\` |
| BioShock 2 Remastered | `%LOCALAPPDATA%\BioshockVR\bs2\` |
| BioShock Infinite | `%LOCALAPPDATA%\BioshockVR\bsi\` |

`bioshockvr.log` is the current run; `bioshockvr.prev.log` is the run before it. When
reporting an issue on GitHub, attach both. Crash dumps, when there are any, land in the
`crash\` subfolder.

The first lines to look for:

- `xr: instance created, runtime: <name> <version>` - VR is working; the runtime named
  here is the one the mod talks to.
- `xr: no 32-bit OpenXR runtime reachable (-51)` or `xrCreateInstance failed` - read on.

## "no OpenXR runtime - flat mode"

These games are **32-bit**, and 32-bit games read a different OpenXR registry key than
every other VR title you own. That is why "all my other VR games work" proves nothing
here. The key is:

```
HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime
```

Check what it points to (read-only, changes nothing):

```bash
powershell -NoProfile -Command "(Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenXR\1').ActiveRuntime"
```

- **Virtual Desktop (recommended):** it should print a path ending in
  `virtualdesktop-openxr-32.json`. If it does not: open Virtual Desktop's Streaming tab,
  set the OpenXR runtime to something else, apply, set it back to **VDXR**. That
  re-selection rewrites both the 64-bit and 32-bit keys. Relaunch the game.
- **Meta Link / Air Link:** set the Oculus runtime as active in the Meta PC app; it ships
  a 32-bit runtime.
- **SteamVR:** see the SteamVR section below.

If the key is correct and it still runs flat, check for a broken API layer (next section).

**Reported: OBS can cause this.** Several users have had "no OpenXR runtime" disappear
after **uninstalling OBS Studio** - the mechanism is not confirmed yet, but it has the
shape of a capture hook or a leftover implicit API layer getting in front of the loader.
Try these in order, least drastic first:

1. **Close OBS completely** and relaunch the game - check the system tray, OBS keeps
   running there after you close its window.
2. **Disable OBS's game/VR capture** (Game Capture source, the "Game Capture" hook, and
   any VR-capture plugin), then relaunch.
3. **Check the API-layer key** in the next section for anything OBS-related and disable
   that entry.
4. Only if none of that works: uninstall OBS to confirm, then reinstall it. If it only
   works with OBS gone, please open an issue with your OBS version and plugin list -
   that is a fixable bug and we want the details.

## Crash at launch or `xrCreateInstance failed: -32`: 32-bit API layers

A 32-bit **implicit OpenXR API layer** from another tool can break the loader before the
mod ever reaches the runtime. Known offenders: the ReShade XR layer
(`ReShade32_XR.json`); OpenXR Toolkit and similar overlays have the same failure shape,
and capture tools such as OBS are a suspect in the same class (above).

Look under this registry key:

```
HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit
```

Each value listed there is a layer JSON path with a DWORD: **0 = enabled, 1 = disabled**.
Set the suspect layer's value to `1` (or uninstall the tool), then relaunch. Example for
the ReShade layer, from an administrator Command Prompt:

```bash
reg add "HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit" /v "C:\ProgramData\ReShade\ReShade32_XR.json" /t REG_DWORD /d 1 /f
```

(Adjust the path to match the value name you actually see in the key.)

## In the headset, but it is a flat floating screen (no stereo, no head tracking)

**Different problem from the one above** - here VR started fine, so the log has no
runtime error at all. What you are looking at is the mod's fallback "mono screen" mode:
the game shows on a flat panel in VR, your head does not move the in-game camera, and
there is no depth.

Confirm it in the log - these lines at startup are the symptom:

```
[bsi][reentry] stereo off            (or [bs2]/[reentry] on the other games)
vrstereo off - back to the mono quad (session stays live)
drive: lane=off ... headOff=(0.0 0.0 0.0)
```

**Cause: your saved `vrpreset.ini` has VR stereo and head-drive switched off**, and it
overrides the shipped defaults on every launch - including after an update, which is why
reinstalling the mod does not help. It usually happens after pressing Save in the F10
menu at a moment when VR happened to be disarmed.

Two ways to fix it:

- **Replace the preset (recommended).** Close the game, then copy the matching
  `preset-bs1\` / `preset-bs2\` / `preset-bsi\` `vrpreset.ini` out of the release zip
  over the one in your data folder (paths in the table at the top of this file), and
  relaunch. Deleting your `vrpreset.ini` outright works too - the built-in defaults then
  apply. The shipped presets are the calibrated ones that get tested.
- **Arm it live.** Press **F10** and tick **VR stereo** and the **camera / head drive**
  options (BioShock 1 and 2: the **VR PRESET 1** / **APPLY PRESET** button does the whole
  sequence in one click), then save if you want it to persist.

While you are there, two settings worth checking in the same file: a **near-square
resolution** (e.g. 2064x2208 - not 2560x1440 or any 16:9 value, see the resolution
section below) and **windowed** rather than exclusive fullscreen.

## SteamVR / Steam Link: supported through the bundled shim

SteamVR's release channel does not ship a 32-bit OpenXR runtime, so the mod cannot talk
to it directly. Since v0.8.x the zip bundles a compatibility shim that fixes this:
**`bvr_steamvr32.dll` + `openvr_api.dll`**, installed next to the game exe like the other
mod DLLs. Nothing to configure - when no native 32-bit OpenXR runtime works, the mod
automatically falls back to the shim, which talks to SteamVR over its OpenVR interface
(fully 32-bit capable). This covers Index, Vive, WMR and Steam Link setups.

The log tells the story:

- `xr: native runtime unavailable - falling back to the SteamVR shim` then
  `xr: instance created on runtime 'BioshockVR SteamVR shim (OpenVR)'` - the shim is
  live. It writes its own log to `%LOCALAPPDATA%\BioshockVR\ovrshim.log`.
- `xr: SteamVR shim also failed ... (is SteamVR installed?)` - the shim could not reach
  SteamVR; make sure SteamVR is installed and the headset is connected.
- `xr: WARNING - game is running elevated` - Windows hides the shim from admin
  processes. Launch the game (and Steam) non-elevated.
- `!!! eye N INVALID projection` in `ovrshim.log` - your headset reports a projection
  outside the documented OpenVR convention; please open an issue with both logs.
- Black headset right after changing resolution - check `ovrshim.log` for a
  `render: eye targets ... (app swapchain changed)` line; if it is missing, open an
  issue with both logs.

Two behaviors to know about: while the **SteamVR dashboard is open, controller input is
paused** (the game sees an unfocused session - close the dashboard to resume), and
quitting SteamVR mid-game drops the game back to flat rendering (it keeps running;
restart the game to re-enter VR after SteamVR is back).

**Menu button opens the Steam overlay instead of the game menu?** Steam Link sometimes
swallows the left menu button before the game sees it. Press **X and Y together (left
controller)** - the chord is the menu button: tap = pause menu, hold = the back/select
action, exactly like the real button. It works on every runtime, not just SteamVR.

To force a path, create `%LOCALAPPDATA%\BioshockVR\xr.ini` containing:

```
[runtime]
mode=steamvr
```

`mode=auto` (default) tries the native runtime first; `mode=native` never uses the shim;
`mode=steamvr` skips the native runtime entirely.

Controller coverage under SteamVR: Quest/Touch and Index have full bindings; Vive wands
and WMR have partial defaults (no face buttons exist - jump/heal/reload are unbound out
of the box). Any of it can be rebound in SteamVR's own controller binding UI, which
always wins over the shipped defaults. WMR bindings have not been verified on hardware.

Quest-specific notes: switching Virtual Desktop's OpenXR runtime to "SteamVR" does
**not** move these 32-bit games off VDXR - they keep using VDXR, which is the better
outcome anyway (native, faster). To actually play through SteamVR on a Quest, use
**Steam Link** (verified working). BioShock Infinite needs a v0.8.1+ build for SteamVR:
older builds left the headset in the SteamVR void with dead input (the game's DXGI 1.0
graphics setup broke SteamVR's texture sharing; the mod now upgrades it at launch - the
log line is `[bsi] gfx: game's CreateDXGIFactory upgraded to CreateDXGIFactory1`).

## Advanced: pointing the loader at a runtime with `XR_RUNTIME_JSON`

The OpenXR loader honours the `XR_RUNTIME_JSON` environment variable **before** the
registry. To force a specific runtime, set it to that runtime's **32-bit** manifest, e.g.
in Steam: right-click the game > Properties > Launch Options:

```
cmd /C "set XR_RUNTIME_JSON=C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr-32.json && %command%"
```

The available runtime manifests on your machine are listed under
`HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\AvailableRuntimes`. Two traps: the variable
must name the 32-bit manifest (not the 64-bit one), and **elevated (admin) shells
silently ignore it** - set it in the same non-elevated context that launches the game.

## Resolution and windowed mode

The eye render IS the game's backbuffer, so the game's own resolution is the VR
resolution. A headset eye is near square - set something like 2048x2048 or 2704x2704,
not 16:9. **Run the games windowed**; exclusive fullscreen is the untested lane and the
known source of "the resolution never changes".

- **BioShock 1:** use the F10 resolution picker, then **restart the game** (the engine
  reads the config only at startup; a live change is not possible on this engine - its
  own `SETRES` command crashes). If the value does not survive a restart: the game
  rewrites `Bioshock.ini` from its own settings at exit and can offer to "revert
  options" at boot - answer **No** to keep your change, or close the game first and edit
  `Bioshock.ini` by hand (`WindowedViewportX/Y` + `FullscreenViewportX/Y` under
  `[WinDrv.WindowsClient]`, in `%APPDATA%\BioshockHD\Bioshock\`), then launch. Set
  `StartupFullscreen=False` while you are there.
- **BioShock 2:** the F10 picker applies **live** (borderless window resize) - windowed
  is the tested lane; if the window has chrome afterwards, use the picker's "Restore
  window chrome" / re-apply once.
- **BioShock Infinite:** the F10 picker applies **live** through the engine's own
  `setres`, fullscreen or windowed, and persists via the config keys.

## Turning off auto-start VR

The mod arms VR by itself on the first frame, so the game starts in VR with no F10 trip.
If that ever misbehaves on your setup, there are two switches - they are the same setting:

- **In game:** F10 > the preset block at the top > untick **"Auto-start VR at launch"**,
  then press **Save preset values** / **SAVE all settings**.
- **With the game closed** (use this if a launch is unusable): open `vrpreset.ini` in the
  game's data folder (table at the top) and set `autoVr=0`. Add the line if it is missing.

With it off, the game starts flat and **VR PRESET 1** / **APPLY PRESET** in the F10
overlay arms everything exactly as before.

## Still flat? Clear the mod's settings

Close the game, then in the game's data folder (table at the top) delete or move
`vrpreset.ini`, `hands.ini`, `weapons.ini` and `command.txt` if present, and relaunch.
Saved settings override the shipped defaults key by key, so a value written by an older
version keeps applying until it is cleared. Pressing **VR PRESET 1** (F10) restores a
working configuration.

## None of that worked

Open an issue at https://github.com/mohamad-balouza/bioshock-vr/issues and attach
`bioshockvr.log` **and** `bioshockvr.prev.log`, plus a note on your headset, streaming
app (Virtual Desktop / Link / Steam Link) and the runtime selected in it.
