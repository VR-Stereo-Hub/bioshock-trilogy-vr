# BS1 F10 settings menu

Implementation and offline verification, 2026-10-08. The separate hand-compatible
build was subsequently authorized and installed. No game launch or headset
acceptance is claimed.

## Installed build

The installed DLL is `v0.8.3-201-g358e08c`, the committed hand implementation at
`f318f3d` plus this menu in the isolated `codex/rapture-f10-hand-build` branch.
The original hand checkout's later `039bb69` changes documentation only; its
`src` tree matches `f318f3d` exactly. The staging-only DLL was not installed.

Installed SHA256:
`5FFF5C4844CB632DB3C20E07658534F81270484010234BD635CB576E1891D838`.
The previous DLL, loader, current and previous logs, and all 21 INIs were backed
up under the compatibility checkout's `build/install-backups/20261009-011511-240-pre-selector-label`
(UTC timestamp). The installed DLL matches its build; a full comparison found
all 21 INIs byte-identical, including their original line endings. The loader
was not replaced. No game or simulator was launched.

## Presentation

The menu uses the approved Rapture artwork with native ImGui interaction. Its
tabs form one close-spaced row with thin brass borders. Narrow layouts scroll
the strip and provide a tab-list picker. Sections start collapsed. Labels, pipe
centers and numeric values occupy one row; a shared value column aligns the
rails. Wrapped labels remain vertically centered. The fixed text-size footer
uses the same columns, while the settings body scrolls independently.

`menu_model` defines stable setting keys, scopes, tiers and applicability.
`menu_view` and `menu_theme` are linked into both the DLL and `bvr_f10_preview`;
the preview is not a separate approximation of the menu. `menu_backend` binds
the model to the existing module settings. Other games keep their old adapter UI.

## Placement and resolution

The reference is the current Dishonored `src/core/ui/overlay.cpp` implementation,
including its resolution-change fix, not the older release checkout. Default
placement is the same eye-image fraction: position 1027,1021 and size 649x685 in
a 2750x2850 eye. This is an overlay drawn into the eye image, not a newly added
world-space quad.

Geometry and typography scale with eye width. The saved text-size preference is
a relative multiplier. Resolution changes rescale the actual current position
and size, including a panel the player moved or resized. Normalized placement
therefore survives resolution changes. Minimum readable size and viewport
constraints still apply to extreme aspect changes. The viewport uses the current
acquired backbuffer dimensions on BS1's opted-in path, and controller pixel
coordinates use that same viewport rather than a prior XR swapchain size.

## Player settings and scope

| Tab | Basic | Advanced additions |
| --- | --- | --- |
| Hands | Hand/model size, weapon size, profile position and rotation | Grip pivot and applicable legacy animation/visibility preferences |
| IK | Shoulder position, arm length and elbow direction | Same useful adjustments; experimental legacy solver knobs are Debug |
| Aim | Reticle and aim laser | Laser shape, visibility rules, profile alignment and origin |
| Controls | Snap/smooth turning, controller shortcuts, motion wrench | Deadzone and swing attack delay |
| Comfort | Movement direction, camera motion, world scale | Viewpoint and cutscene behavior |
| HUD | Separate HUD panel | Placement and size |
| Display | Gameplay FOV | Flat screen placement, size, distance and startup preference |

Runtime and Diagnostics appear only in Debug. Inapplicable implementation knobs
are hidden, rather than merely collected under Debug. Mode 4 hides the old
actor-replay and legacy solver controls. The v2 solver also hides the retired
twist/follow/smoothing controls. The unrelated script-seam experiment is omitted.

Per-profile model/aim edits require the matching equipped hand and a known profile.
Changing equipment before a queued edit is applied discards that stale edit.
The Left/Right/Both selector affects only hand-scoped controls. Global values
remain global. A disabled control explains the requirement next to it.

The hand branch at `f318f3d` introduces a shared shoulder center, shoulder width,
linked shoulders, and one hand-and-arm size multiplier. The menu exposes these
in mode 4 instead of the obsolete independent shoulder positions. Session 85
on that branch removed linked shoulders (the shoulders now stay where they are
set; HANDS_DISHONORED.md, the s85 audit) and added `WeaponFollowsHands`, on by
default, beside `HandsArmsSize`.

Session 86 made mode 4 the only drive (`HandMode` has one entry; the
`LegacyHands`/`LegacySolver` controls never show) and added, all `ComposedHands`:
per-hand `HandForward/Right/Up` (view frame), `HandPitch/Yaw/Roll`,
`HandGripForward/Right/Up` (palm frame) under Hands - persisted since s87 as `HandPalmTrimForward/Right/Up`, so a grip saved for the wrist anchor (it put the palm 9.5 cm off the controller) is dropped rather than reloaded; `ShowArms` (Hands > Arms);
`UpperArmLength` and `ForearmLength` under IK. The shoulder keys became
`ShouldersForward/Right/Up/Width`, measured from the eye, so saved
`ShoulderBar*` values are ignored as unknown. Defaults are the Dishonored fit
(HANDS_DISHONORED.md s86). The regular
staging build has no mode 4 and does not expose those controls. Compatibility
with the hand branch requires that commit's public APIs or newer equivalents.

## Persistence and threading

Player edits are queued for the game thread. Toggles and choices commit at once;
sliders commit on release. Pending values remain visible while the game thread
has not drained the queue, so releasing a slider while paused cannot replace its
final sample with an old module value. The footer reports pending or failed saves.

`menu-settings.ini` in the mod data folder stores player choices separately from
legacy files. Its versioned text schema is `P/D key hand profile value`, with
CRLF, stable keys, finite/range validation and scope validation. It is not a
Windows section/key INI schema. A successful replacement keeps `.bak`; a failed
write leaves the previous file intact. Runtime > Settings storage has Retry saving.

Player preferences are reapplied after startup defaults and the VR recovery
preset. Profile preferences are reapplied after the legacy weapon profile.
World scale is restored before converting centimeter offsets. Debug changes are
session-only unless the explicit Save debug defaults confirmation is used.
Existing `hands.ini`, weapon profiles and `vrpreset.ini` are not rewritten by
this menu. Removing `menu-settings.ini` while the game is closed removes its
overrides. Engine objects and retained engine pointers are not added to the UI;
the bindings call existing settings setters or module atomics.

The resolution panel is deliberately status-only: the old live config write can
be overwritten by the game's exit save. This menu does not promise a working
next-launch resolution action without implementing a safe path for it.

## Verification

Built RelWithDebInfo/Win32 with MSVC 19.51 and Windows SDK 10.0.26100.0 on the
staging base `d5a8066`, and separately with the committed hand branch `f318f3d`.
The hand work's original checkout and uncommitted changes were not modified.

The offscreen executable renders the production menu through D3D11 using a
fixture backend. Invocations take width, height, geometry scale, text multiplier
and output directory. No game, OpenXR session, hooks or user preferences are
loaded by the executable. Representative cases:

```
bvr_f10_preview.exe 760 850 1 1 build/f10-renders/standard
bvr_f10_preview.exe 1064 1106 1.4 1 build/f10-renders/approved-size
bvr_f10_preview.exe 640 760 1 1.25 build/f10-renders/large-text
bvr_f10_preview.exe 640 760 1 1.5 build/f10-renders/max-text
bvr_f10_preview.exe 649 685 0.85394737 1 build/f10-renders/live-reference
```

All five cases pass 327 checks each. Checks cover all player and Debug pages, rail/label vertical alignment, footer
bounds, mouse button/slider input, save on release, keyboard tab overflow,
Basic/Advanced/Debug visibility, profile gates and preference serialization.
The live placement path is also exercised in one ImGui session at 2750x2850,
4763x4936 (approximately 300% pixels), 5500x5700 and 1920x1080, including a
resolution roundtrip and a user-moved/resized panel. Font size, hitboxes and
saved text preference are checked independently of the panel rectangle.

All six artwork hashes match the approved draft. The native screenshots were
visually inspected against that draft, with subsequent requested refinements:
single-line close-spaced tabs, brass outlines and inline aligned pipe sliders.

This verifies the native menu and its pure preference model, not engine-side
effects, filesystem failure behavior in a running game, controller comfort or
headset readability. Those remain an installed-build playtest. Do not
install the staging-only DLL over the in-progress hand build.
