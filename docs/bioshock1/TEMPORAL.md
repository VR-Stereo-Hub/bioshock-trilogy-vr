# BS1: DLSS / DLAA, TAA and the alternate frame warp

## Accepted baseline (2026-10-12)

AFW uses one freshly rendered eye per game frame and reconstructs the other eye
with the accepted v307 mesh path. The same reconstruction works with DLSS on or
off. The final headset comparison was accepted with native foreground replay
turned off. This is the baseline for the completed AFW development cycle.

Select **Alternate frame warp (AFW)** under F10's stereo controls. Headset
acceptance covers **DLSS Super Resolution / Transformer K** and AFW with DLSS
switched off. Quality is selectable independently in F10. Sequential stereo is
still the startup default when no saved AFW choice exists.

Existing diagnostic INIs should use:

```ini
[Bioshock1R]
NativeForegroundRepair=0
```

Zero is also the source default. This prevents the extra native hand/weapon and
HDR replay from replacing the mesh result when DLSS is off. The diagnostic
modules remain opt-in; they are not the accepted AFW rendering path.

The accepted setup also enabled `NonBlockingDesktopPresent=1` in the same INI
section. That optional setting avoids a desktop-present wait on focused VR
projection frames; it does not change AFW image reconstruction. Its source default
remains off. Keep existing hand, weapon and headset calibration when applying
these settings.

**Known limitation:** broken pixels can remain around hand and weapon edges.
The accepted baseline does not claim to eliminate them. GPU cost also depends
on the render and output resolutions, so DLSS-off and DLSS-on are not a promise
of identical frame rates.

The sections below preserve the earlier implementation and test history.
Current acceptance and next steps are recorded at the top of `docs/STATUS.md`.

## Current controls and setup

1. In **F10 > Display > Rendering method: Stereo or AFW**, choose the stereo method:
   **Both eyes every frame** is sequential stereo; **Alternate eye (AER)** renders
   the two eyes as a pair; **Alternate frame warp (AFW)** presents every game frame
   and reconstructs the eye that was not freshly rendered.
2. In **Upscaling and anti-aliasing (DLSS)**, select Off, DLAA, DLSS SR or TAA.
   DLSS modes require the NVIDIA helper and a supported GPU; TAA does not.
   Missing or failed helper output falls back to the normal image path.
3. **Mode** chooses Ultra Quality, Quality, Balanced, Performance or Ultra
   Performance. **Model** is an Advanced control. These settings apply live;
   DLSS SR adjusts the game's internal render size automatically. The Resolution
   section controls output size separately.
4. Keep `NativeForegroundRepair=0` for the accepted AFW path. Existing F10 choices
   and hand/weapon calibration are retained when changing the rendering method.

For a development build, initialize the pinned Git submodules and build the mod
with `tools/build.ps1 -Release`. DLSS additionally needs the SDK fetched by
`tools/fetch-ngx.ps1` and the helper built by `tools/build-dlss-host.ps1`.
`tools/install.ps1 -Release -Game bs1` copies the helper when present. The SDK and
runtime binaries are not committed to the repository; the helper's required
license and notices are copied alongside its runtime by the build/install scripts.

The existing `dlss`, `aer` and `afw` diagnostic commands remain available. These
commands and Debug controls do not change which reconstruction is the accepted
baseline. The historical sections below describe earlier experiments and may
mention settings or launch steps that have since changed.

## The first headset run (2026-10-10, 09:15) and what changed

AFW on: the eyes flickered between the left and right positions, then the game crashed in the
NVIDIA driver 5 s later.

**The flicker - found and fixed.** The engine calls CalcView about four times a frame (the log:
`calcview in=130 out=398/s`; BRVR measured 4.2): the render view inside the scene build, and
three movement/AI/audio consumers outside it. The first AFW build flipped the eye on EVERY
call and queued an eye tag for every call (a 4-deep queue that dropped its oldest), so the
image each present named, and the eye the frame actually rendered, were unrelated. BRVR only
ever tags its "leader" call site; Dishonored pushes one tag per scene draw. Now the eye is
decided ONCE per frame, at the render view's CalcView (`scenedraw::in_gameplay_build`), every
call of the frame carries it (the foreground scene node takes its camera from the calls BEFORE
the build's own - ENGINE_NOTES session 21), and exactly one tag per frame is pushed. The
integration suite now drives the real four-call pattern and fails the old code.

**The crash - not explained.** `0xC0000005` in `nvwgf2um.dll+0x48153A` (a read at 0x9370) on a
thread whose live stack is all NVIDIA driver (`tools\read-dump.py` needs a Python package this
machine lacks; a stack scan of the dump found no mod or game frame live on it): a driver worker
executing commands queued earlier. No minidump memory list, so address-space exhaustion
cannot be ruled in or out. Checked and cleared: every texture the tap copies is AddRef'd, the
tap is re-entrancy guarded, all state our passes touch is restored, no swapchain image is
released unwritten. If it recurs with the fixed eye logic, the next step is a RenderDoc
capture of one AFW frame and `watch-crashes.ps1 -Mode Memory`.

**What the run proved** (no further headset time needed for these):
- the vector audit: `vectors 0.0071, no motion 0.0171`, depth scale `1` lowest - the motion
  vectors and the 10 uu near plane are right in the real game. The audit is now OFF by default
  (it copies every eye image each frame); `dlss audit on` brings it back.
- the pass structure `160W 8F 115F 32F 10F 22F 3F`: SIX segments after the world on its depth
  surface. The tap called every one of them the foreground whether or not a depth clear came
  first - and without a clear, the "foreground" copy is the whole world's depth, so the hands
  mask covered the entire image (`with a hands pass 648` of 648 fits). Fixed: a later segment
  is the foreground only after the world surface's depth was CLEARED; before that it is more
  of the world and refreshes the world copy (`w`). The log now marks clears with `|`, so the
  next run shows the real structure (ENGINE_NOTES session 21 says the foreground draws FIRST
  in the main pass, which would put the hands inside `W` and leave no separate mask).

## The second headset run (2026-10-10, 19:28) and what changed

AFW worked, with a smaller ghost of the hands (the tester: "similar to Dishonored's FOV
mismatch"); the game crashed again ~10 s into AFW, at the same driver address.

**What the run settled.** The hands have no pass of their own: `381W 8w 8w 4w 261w ... 3w |` -
no depth clear until after everything, so they are drawn into the world's depth (ENGINE_NOTES
2026-10-10 table). The foreground FOV field held its lens-matched 117.46 from boot through the
crash, and the FOV watch never saw a second lens.

**The ghost - measured, not yet explained, three controls shipped.** BS1 does have a separate
foreground FOV, but in this build it is matched to the world: two frame dumps give every
foreground draw (the hands draw included) the world's tangents AND its 10 uu near plane, so the
hands' depth is consistent with how they are drawn - not Dishonored's foreground gain (0.911,
guide 9.5) by the dumps. Three candidates remain, each with an F10 control:
1. A depth/disparity mismatch on the hands the dumps cannot see: the **hand-depth probe**
   (F10 Debug "Measure the hands' depth", Stereo method both eyes every frame) block-matches
   the two native eyes' hand pixels against what their depth predicts and logs the ratio every
   5 s (`[temporal] hand-depth probe`); **AFW: hand depth gain** (F10 Debug, `afw fggain`)
   applies it to everything nearer than 80 uu in the rebuild. Offline: the probe reads 1.00 on
   true depth and 1.12 on hands 12% too deep; gain 1.12 undoes that error exactly.
2. Which source the ghost comes from: **AFW: tint the rebuilt eye by source** - blue = from the
   other eye, magenta = fill, natural colour = the held eye's own last image.
3. Dishonored's cadence beat (ENGINE_NOTES there, 2026-09-04): the headset ran at 144 Hz
   (6.94 ms) and AFW at ~130 frames/s = 1.11 display slots per frame, the ratio Dishonored
   measured as ghosting (1.02 clean, 1.05 visible). A/B: the same scene at 120 or 90 Hz.

**The crash - narrowed.** Same address both runs (`nvwgf2um.dll+0x48153A`, a read at 0x9370, a
driver worker thread), 5 and 11 s into AFW. A new suite, `afw-soak`, runs the rebuild at the
tester's 2750x2850 in a 32-bit process on this machine's driver for 30 s (~1,900 rebuilds, depth
steps, a near hand, the stale test): **no crash** - the shaders and draw sizes alone do not
trigger it. Shipped: **AFW: rebuild the other eye** (F10 Debug, `afw rebuild off`) - off keeps
AFW's one-eye-per-frame path and leaves the held eye to the headset, so one run tells whether
the rebuild or core's two-swapchain AlternateEye path is involved; and two hedges with no
change in output: the mesh is drawn in bands of at most 1M vertices (it was one 11.8M-vertex
draw; the bands are bit-identical to one draw, tested - and `SV_VertexID` measured to restart at
0 per draw, so each band's base rides in the constant buffer), and no dynamically indexed
constant arrays remain in the shaders.

## The third headset run (2026-10-10, 20:04) and what changed

The game booted straight into AFW (the stereo method persists) and ran without a crash; each
hand showed a second, transparent copy nearer the middle, flickering - and opening the F10
panel made the hands normal every time. Turning on the probe (while still in AFW) was
followed by the crash.

**The panel and the ghost.** While the panel is up AFW stops rebuilding the held eye, so the
panel's "fixed" view IS AFW without the rebuild: the rebuild draws the ghost hands. The log
also shows the rebuild ran for 45 s from boot (~6,000 rebuilds) before the panel was ever
opened.

**The crash - a pattern across all three runs.** None came during the 45 s from boot. Every one
came 1.7-10 s after the panel CLOSED (runs 1 and 2: AFW switched on with the panel up). The
dumps add the other half: the crashed thread was created when the XR session started, ~100 ms
before Virtual Desktop's OpenXR runtime thread, long after the game's device - the NVIDIA worker
of the runtime's own D3D11 device, the one that reads our eye swapchains. With the panel up AFW
wrote only the fresh eye's swapchain (each eye every other frame); closing it went back to both
every frame. **Changed:** in AFW the held eye's swapchain is now written on EVERY present -
rebuilt normally; with the panel up, the fresh eye's image (flat while the panel is open, the
panel readable in both eyes); without a depth this frame, its own last image. Also fixed: the
held eye's store kept its CAMERA current during the panel but not its IMAGE, so the first
rebuilds after the panel paired an old image with a new camera. Two offline tests (a 30 s
soak, and the same with real ImGui DX11 drawing between stretches) did not reproduce the crash
- they have no runtime device, which is where it happens.

**The ghost - the next suspect, instrumented and handled.** A viewmodel is often kept in front of
the world by drawing it into a squeezed viewport depth range (MinDepth 0, MaxDepth 0.1, say).
No draw matrix shows that (the near-plane check above read matrices), but the hands' stored
depth would then decode several times too near and the rebuild would place them wrongly. Added:
the draw tap samples every scene draw's viewport depth range every 8th frame and logs the
census every 5 s (`[temporal] viewport depth ranges of the scene's draws`); when draws use a
squeezed [0, hi) range beside full-range ones, that depth is re-expanded before it is
linearised (`[temporal] draws squeezed into depth ...`; lever `dlss squeeze off`). Offline: the
hand reads its true depth (1.7e-07) with the range, 76% wrong without, the world untouched. The
probe now spans 0.125-2.0, so a squeezed viewmodel is measured (0.24 in the test), not dropped.

## The fourth headset run (2026-10-10, 20:48) and what changed

The census answered the viewmodel question: **the hands and weapon draw into viewport depth
[0, 0.010]** beside the world's [0, 1] (ENGINE_NOTES 2026-10-10); with the re-expansion on, the
ghost when standing still was gone, and five panel open/close cycles in AFW did not crash
(writing the held eye's swapchain every present looks like the fix). Two faults remained.

**Hands and weapon a frame behind, doubled, while moving with the sticks - found offline and
fixed.** The rebuild carried the held eye's own hands forward as WORLD geometry. They are not:
they move with the camera and the controllers, so standing still the two eyes' copies agree
and moving they do not. Why the offline suite never showed it: its one frame of motion moved
the camera 4 uu, and a hand 4 uu out of place fails the stale test (3% + 2 uu, tuned for the
world) and gives way; one real frame of walking at 130 fps moves it ~1.5 uu, which is UNDER
the tolerance, and the old hand stays. A new case in `afw-gpu` walks one real frame (1.5 uu,
0.4 deg) with a textured hand carried in the head's frame: the old code's hand-region error is
0.114, 7x the masked rebuild's 0.0155 (world unchanged). The fix is Dishonored's rule (its
guide 7.1: the world from the held eye, the hands from the fresh eye), with the mask BS1 has
for free: **a pixel whose raw depth lies inside the squeezed range is viewmodel.** The guides
mark it (`temporal_gpu` `hands`, exact in the test: 945 of 945 hand pixels, 0 wrong), it rides
beside the depth in each eye's AFW store, and in `afw_gpu` the held source's hand texels are
dropped (except where the fresh frame does not contain the point - the arm at the frame edge,
guide 9.7 - where the old hand beats a hole), the fresh source's are flagged and win the
compose outright, a foreground fresh sample never makes the held world stale (the weapon
pushed into a wall, guide 7.3), and the fill extends world seeds only (guide 7.4). The hands
sit in the near half of each source's z-buffer, so they win as the game draws them (guide
9.8). The same mask now feeds DLSS's bias-current-colour input and TAA (before, only a
separate foreground pass could, and BS1 has none). Lever: F10 Debug **AFW: hands from the
fresh eye** / `afw mask off` (= the fourth-run code, the control); `afw tint on` paints them
green.

**The F10 panel flickered the hands - fixed.** Round four wrote the FRESH eye's image into
the held eye while the panel was up, so each eye alternated between its own image and the
other's. The held eye now re-shows its OWN last image and keeps its pose (`held_rebuild`
returns 2, the compositor's rotation-only reprojection), still written every present so its
swapchain never pauses. The integration test asserts it (4 of 4 presents, its own image, never
the fresh eye's).

**Super Resolution grew the eye images past the headset - fixed.** The log showed `2750x2850
-> 4124x4274` (Quality) and `-> 4300x4456` (Balanced): the round-one rule made the output
render x ratio, so the swapchains outgrew the headset (Virtual Desktop's overlay read ~160%),
the game still rendered its full 2750x2850, and DLSS, the AFW rebuild and the compositor all
paid for the larger image (AFW + Balanced ~85 fps). Dishonored's design is now in: **the
output is the headset's resolution** (the runtime's recommended eye size - what the player
chose in Virtual Desktop or SteamVR - at the render's aspect, at most 3x the render, never
below it) **and the game renders below it.** BS1 cannot resize its render live - `SETRES`
through the Exec seam faults (ENGINE_NOTES session 27), and nothing else reads a resolution -
so the render size stays the game's own setting: the F10 status under the anti-aliasing
section says which game resolution the chosen scale wants (output / ratio: 1834x1900 for
Quality at 2750x2850) with a button that writes it to Bioshock.ini for the next launch (the
existing F10 Render-resolution path), and the log says the same once per size
(`[dlss] super resolution: eye output ...`). Until the game renders smaller than the headset,
SR has nothing to reconstruct and the helper builds DLAA. The DLSS quality mode follows the
real ratio the helper sees. Lever: `dlss output <w>x<h>|auto`. Integration test: the output is
the stub headset's size (576x540 for a 384x360 render: the stub now presents a 1.5x headset,
so the SR quality check measures the Quality ratio; at a 3x cap DLSS scored level with bilinear
on that scene).

## The fifth headset run (2026-10-10, 21:51) and what changed

AFW from boot (the method persists), the F10 panel up for most of 90 s, Super Resolution
switched on at 21:53:16 (which REBUILT the eye swapchains at 2750x2850 -> 2750x2850, the same
size), the panel closed at 21:53:19.7, the crash at 21:53:22.5 - the same address as every run
(`nvwgf2um.dll+0x48153A`, a read at 0x9370, a driver worker thread). The tester also saw the
arms huge and oddly projected (not yet separated from the hands branch the test build
carries: the first question for the next run is whether they look the same with Stereo
method "both eyes every frame", which takes AFW out of it).

**The dump, read properly this time** (`read-dump.py`, then every thread's stack scanned
against the build's `.pdb`): the faulting thread holds driver and ntdll frames only; the one
mod address on it is a pointer to a string literal in `.rdata` - data, not a return address.
The present thread was INSIDE the Virtual Desktop runtime at that moment (its frames on top),
with `held_rebuild`'s return addresses (temporal.cpp 1048, 1065, 1066) just below: a rebuild
had just finished and core was in the runtime's acquire/release/end-frame path. So the crash
is a driver worker - the runtime's device's or the game's - executing a queued command whose
object is NULL (+0x9370), seconds after the rebuild resumes (every run: 1.7-10 s after the
panel closed; this run also 6 s after a swapchain rebuild). Nothing in the dump names the
command.

**Changed, in order of what they are for:**
1. **The D3D11 debug layer, armed for one launch** (`core/hooks/d3d11_debug.cpp`; F10 >
   Diagnostics > D3D11 debug layer, or `d3ddebug on|break|off`). A marker file in the data
   dir makes the mod hook `D3D11CreateDevice(AndSwapChain)` ahead of the game's own device
   creation (the mod loads with xinput1_3 at process start) and add the debug flag; each
   present drains the device's info queue into the log, deduplicated by message id and
   capped. "break" also raises a debug break on the CALLING thread for every error, which
   the crash filter turns into a dump with the mod's own frame on it. This is the one
   instrument that names an invalid D3D11 call (a view of a released resource, a resource of
   another device, a bad bind) before the driver faults on it. The SDK layers are on this
   machine (`SysWOW64\d3d11sdklayers.dll`).
2. **No pointless swapchain rebuild**: a Super Resolution output equal to the render is
   asked of core as 0 x 0 (the backbuffer size), so switching SR on at the headset's
   resolution no longer destroys and recreates the eye swapchains.
3. **The AFW capture and replay** (Dishonored's `afw dump` and `afw-replay`, ported): F10 >
   Diagnostics > AFW capture (or `afw dump [n]`) writes N consecutive rebuilt presents - both
   eyes' colour, linear depth and hands mask, the rebuilt image, and a header with both
   cameras, the target, every lever and the sizes - to `%LOCALAPPDATA%\BioshockVR\dumps\`.
   `tools\afw-replay.ps1 -Capture <folder>` re-runs the production rebuild on it with any
   lever changed (`-Mask 0`, `-Gain`, `-Stale 0`, `-Grid`, `-Tint`, `-NoHeld`, `-NoFresh`),
   writes BMPs of everything, prints the hands' depth from the masks (median and 2-98%
   band: a hand at arm's length reads 45-80 uu; several times nearer means the squeezed range
   was not re-expanded) and the camera deltas, and scores each rebuild against the next native
   frame of that eye. The "huge arms" question is answered from a capture, not a guess.

## The sixth headset run (2026-10-10, 22:29): the crash, found

AFW from boot, no panel, the rebuild running from 22:29:55 (612 rebuilds in the first 5 s),
the crash at 22:30:02 "the moment I turned" - the same address, the seventh time. The debug
layer was not armed. This dump, every thread scanned against the build's `.pdb`:
**`frame_inspector`'s `UnmapDetour` was on the stack of Virtual Desktop's compositor thread**
(`VirtualDesktop.LibOVRRT32_1.dll` -> `virtualdesktop-openxr-32.dll` -> `nvwgf2um` -> our
detour). The frame inspector hooks the D3D11 immediate context's VTABLE slots (Draw,
DrawIndexed, OMSetRenderTargets, Map/Unmap, ClearDSV, CopyResource, ...), and **a vtable is
shared by every context in the process** - the OpenXR runtime's own device's contexts
included, on the runtime's threads. So the HUD and GFx classifiers, the draw tap (temporal's
depth capture: `tap_setrt`/`tap_draw`/`tap_clear`, which copy depth surfaces with OUR device's
resources) and the recorder all ran on the runtime's contexts, with unsynchronized state,
from a second thread. A command with another device's resource, or a stale texture pointer
from the cross-thread bookkeeping, lands on the runtime's device's driver worker as a NULL
object - `mov ecx, [eax+0x9370]` with `eax` loaded from a null context field - which is
exactly the fault, and it needs the runtime busy with our swapchain images (AFW: both eyes'
images rewritten every present, the compositor reprojecting) seconds after the tap and the
rebuild start. The 30 s offline soaks could never show it: no runtime device there.

**Fixed in core (`frame_inspector.cpp`)**: `install` now takes the game's immediate context;
every detour does its work only for the game's device's contexts (the immediate one by
pointer, deferred ones by one `GetDevice` compare per new context, remembered) and passes
any other context straight to the original. The log names each new context's owner once.
A second real bug alongside it, in `afw_gpu`'s state save: only two render targets were
fetched and re-bound, unbinding slots 2..7 of whatever the game had bound at Present - now
all eight.

## The seventh headset run (2026-10-10, 22:51): no crash; the hands, from a capture

No crash in 6 minutes of AFW with the panel opened and closed six times - the foreign-context
guard holds. The tester: the arms jittery under AFW, "the same problem as before" with the F10
panel up, and Super Resolution "not working at all". He took an AFW capture (8 presents,
standing still); `tools\afw-replay.ps1` on it, with the pictures converted to PNG and looked
at, settled the arms:

- **The hands mask is right and large**: 23.5% of the image (the arms reach in from both
  bottom corners), exact against the picture. Their stored depth: median 16.8 uu, 2-98%
  10.5-45 uu.
- **The rebuilt arm had tears and streaks along every sleeve edge.** Tinted: those strips were
  the fresh eye's STRETCHED silhouette sheets drawn as the last resort, because the held eye's
  own hand pixels were gone there. Two faults: (1) the held source's hands were dropped
  wherever the fresh frame contained the point, even where the fresh eye sees it HIDDEN behind
  its own nearer surface (the far side of the arm) - Dishonored's guide 9.7 second half, now
  built: a held hand pixel is kept where the fresh eye's depth at its projection is nearer by
  more than 1% + 0.25 uu; (2) in the compose any real fresh pixel outranked a kept held hand,
  so the fresh eye's WORLD behind the arm painted the arm's edge - now the normal nearer rule
  decides, and the held hand (near) beats the world (far) behind it.
- **The hands' geometry is 1.55x off, and that is the jitter.** The capture holds a near-native
  pair (this present's fresh eye and the next present's, 7 ms apart, the head still): the
  replay's disparity probe block-matches the two along the rows. Hand pixels carry **0.645x**
  the disparity their stored depth predicts, in all seven pairs (0.614-0.661); world pixels
  carry 1.0 (the control, 0.96-1.07). So each eye's hands were reprojected into the other
  with 1.55x too little parallax, the two eyes disagreed by ~150 px at the sleeves, and each
  eye alternated between its native hands and the other's reprojection: jitter. Dishonored's
  run 5-6 finding exactly (its 0.911). Depth and lens are indistinguishable in a disparity
  ratio; the replay decided it: a depth gain of 0.645 on the hands (depth / 0.645: a
  viewmodel near plane of ~15.5 uu against the assumed 10, by the shape) scores 0.0435 in the
  hand region, the lens match's own tangent ratio 1.38 as a lens gain 0.0454, 1.55 as a lens
  gain 0.0445, both 0.0479, and the tinted pictures with the depth gain show the held eye's own
  sleeve continuing seamlessly into the reprojected one. **Shipped: hand depth gain 0.645 by
  default** (F10 Debug "AFW: hand depth gain", range now 0.25-2; re-measure after any hands
  change with F10 Debug "Measure the hands' depth" or the replay's probe line), the hands'
  lens gain as a lever only (`afw lens <x>`, `-FgTan` in the replay; `afw_gpu` carries hand
  texels on their own tangents when it is not 1).
- **Super Resolution at the headset's resolution did nothing at all**: the output equalled the
  render and the size gate refused the evaluate, so neither SR nor DLAA ran. Now it runs as
  DLAA until the game renders smaller (the status says which resolution to set).
- The capture's own stall (120 MB of readbacks per present) aged the held source past its
  150 ms window on every other present; a capture now allows 2 s.
- The replay tool: the tint now also paints the held eye's kept hands yellow and stretched
  sheets orange (A) / cyan (B); the disparity probe prints per present; `-FgTan`.

## The eighth headset run (2026-10-11, 10:44): the arms, found; the window door, shut

The tester: the arms huge "like giant hands", flickering hard under AFW, and with the F10
panel up (no rebuild) the hands right but the ARMS "rendering mono" - the same thing as every
run before. A second capture, replayed: the rebuilt right sleeve turned BLACK across its outer
half (the fill found nothing within 64 px of a 23%-of-the-image sleeve), and the disparity
probe read the hands at 0.55-0.60 of their predicted parallax against 0.645 the night before -
a factor that moves with the pose is not a lens and not a near plane.

**Historical hypothesis, disproved by run ten:** this session attributed the arms to
a one-build-stale foreground camera and extended the ctor substitution to alternate
methods. It assumed the constructor ran before CalcView. Static analysis after the
tenth run shows CalcView first, followed by the player-scene constructor; the old
`current_sign()` therefore selected the next eye, and enabling it crossed the whole
scene. This paragraph originally called that hypothesis a proven cause. It was not.
The hand gain remains 1.0; the old 0.645 ratio is not sufficient to choose a correction.
See the tenth-run entry and ENGINE_NOTES for the reproduced regression and fix.

Alongside: the compose's last resort is now the held eye's own pixel at that position (what
the compositor would have shown) instead of black. And the window route to a live resolution
change is shut: the engine ignored the window's new client size (no SetRes, no ResizeBuffers)
and Windows clamped the window to the desktop height anyway (the client was already
2750x1415 under a 2750x2850 render). `resize.cpp` now does what SETRES does, minus the Exec
seam that faulted: the render device's SetRes on the live viewport (viewport +0xA4, vtable
slot 61, the viewport's reentry counter held), SEH-isolated, confirmed by the backbuffer;
`vrres live window <w>x<h>` keeps the window route for a machine where it might work.

## The ninth headset run (2026-10-11, 11:27): the resize works; what the captures said

The engine's SetRes door works: `[resize] CONFIRMED` for 3072x3072, 1536x1600, 1834x1900,
2048x2048 and back to 2750x2850, each followed by core's swapchain rebuild and, with Super
Resolution on, `1536x1600 -> 2688x2800 (DLSS Super Resolution)` and `1834x1900 -> 2688x2784`
(the output is the headset's recommended size at the render's aspect). The device's vtable
read `+0xE38E7C`, SetRes `+0x75F940`, prologue `55 8B EC 6A`, on every call. The tester
calls DLSS a huge benefit and the controls confusing; three faults, two captures (both
analysed and deleted, per the new rule in CLAUDE.md):

- **The arms "slightly transparent and flickering", fine with the panel up.** The capture's
  probe still reads the hands at 0.638 of the predicted parallax (the world 0.98): the
  ratio seen earlier. The log had no `[fgnode]` line because view-sync defaulted OFF.
  At the time this was interpreted as a fix that never ran. Run ten shows that the
  proposed camera cause was unproven and enabling the wrongly timed substitution
  introduced a separate whole-scene regression. The tinted replay shows the held eye's own sleeve (yellow) displaced from the
  fresh eye's (green): that sleeve, every other frame, is the transparent flicker.
- **"Super aliased" edges with DLSS or AFW on, gone with the panel up.** The same 300x300
  window of a skylight frame in the capture: the native DLSS eye smooth, the rebuilt eye's
  bars stepped in 2-px cells and the thin truss behind them torn to fragments. The mesh at
  grid 2 quantises every silhouette to its cell and loses anything thinner; the rebuilt eye
  alternates with the native one, so every edge shimmers. Dishonored's seed-map plus
  per-pixel refinement (its guide, section 6) is the answer.
- **The panel pixelated under Performance, and the aliasing back while it is up.** The
  panel is drawn into the backbuffer before the eye capture (upscaled with the render), and
  DLSS, TAA and the rebuild are suspended while it is visible.



## 2026-10-11: the render resolution changes live, so Super Resolution needs no restart

The Dishonored mod changes its game's render size live by calling the engine's viewport
Resize (its `viewport_resize.cpp`, an ABI-checked direct call on the game thread) and drives
DLSS SR with it: the output is the headset's size, the render becomes output / ratio. BS1's
`SETRES` faults through our Exec seam, but headless IDA (ENGINE_NOTES 2026-10-11) showed the
engine resizing ITSELF on `WM_SIZE`: windowed, a device bound, a size that differs from the
current one - the window procedure calls the render device's SetRes, the very call SETRES
makes. So `bioshock1r/resize.cpp` resizes the game WINDOW's client area with `SetWindowPos`
from the game thread and lets the engine do the rest (SetRes -> `ResizeBuffers` -> core's XR
swapchain rebuild), confirming by the backbuffer size within 3 s (`[resize] CONFIRMED` /
`NOT TAKEN`). F10 > Display > Resolution: a size list and **Apply now (live)**; the Super
Resolution status: **Render at that size now**; `vrres live <w>x<h>`. Not run in the game yet:
a window larger than the desktop may be clamped by Windows (the engine would follow the
clamped size; the log says so). If it is, the fallback is Dishonored's: the device's SetRes
called directly (viewport +0xA4, vtable +0xF4, the arguments in ENGINE_NOTES).

## How it works

| Stage | Where | What |
|---|---|---|
| Depth | `bioshock1r/temporal.cpp` via `core/gfx/frame_inspector` `set_draw_tap` | The draw tap follows the game's depth binding. Per present interval, the backbuffer-size D24 surface with the most `DrawIndexed` calls is the WORLD pass and is copied before it is cleared or unbound; a later pass on the same surface (after the world's clear) is the FOREGROUND - the arms and the weapon - and is copied beside it. |
| Cameras | `camera.cpp` -> `temporal::note_camera` | CalcView publishes the final camera of every build (after every offset). SequentialReentry images take the latest camera of their eye; AlternateEye images carry the exact build through a FIFO. |
| Guides | `bioshock1r/temporal_gpu` | Depth (+ the foreground's where it drew) -> linear view depth, DLSS depth (reversed), camera motion vectors (previous UV minus current), and the hands mask (a foreground pass's pixels, or - BS1's case - raw depth inside the viewmodel's squeezed viewport range, [0, 0.010)). Hand pixels near a tracked grip carry that controller's rigid motion (see "Hand motion vectors" below). The vector audit scores them in-game. |
| DLSS | `core/gfx/dlss_client` + `src/tools/dlss_host` | An x64 helper owns NGX (310.7, preset K by default), one feature and history per eye, textures and fences shared by NT handle. The hands mask is DLSS's "bias current colour" mask. |
| TAA | `bioshock1r/temporal_gpu` `TaaGpu` | Per eye: history fetched along the vectors, clamped to the current 3x3 range in YCoCg, blended at 0.12 (`dlss taaweight`). Hands, no history or off screen = the current image. |
| Output | `core/vr/openxr_runtime` eye-processor seam | The result replaces the backbuffer copy into that eye's swapchain. Super Resolution asks core for eye swapchains at the HEADSET's size (the runtime's recommended eye size, at the render's aspect, at most 3x the render: `set_eye_output_size`; `dlss output <w>x<h>` overrides) and the game renders below it; anything not reconstructed is stretched. |
| Eye sequencing | `bioshock1r/alt_eye` | One eye per FRAME, decided on the game thread at the render view's CalcView and carried by every CalcView call of the frame; a non-stereo frame restarts the sequence on the left. AER tags each frame into core's SequentialReentry ring (`vr::sr_push_eye`) and pairs are submitted together; AFW names each image through temporal's FIFO (BRVR's underrun/drift resync). |
| AER | `bioshock1r/alt_eye` | BRVR's method: the right frame takes the left frame's head sample (`latch_head`) and each call site's left camera (`pair_lock`); the frame-delta hook (patterns `kFrameDeltaRva`) holds the world on the right frame and pays the delta into the next left frame (no game time lost); core's pair pacing submits L+R as one XR frame with one locate's pose; core's SR ring gets BRVR's drift resync (`set_sr_drift_resync`). |
| AFW | `bioshock1r/afw_gpu` via core's held-rebuild and AER eye-source seams | AlternateEye, every present its own XR frame, world NOT held (Dishonored's rule). After the fresh eye, the held eye is rebuilt at the same instant from both eyes' images and depths: a depth-tested mesh per source, a stale test (a point the fresh eye sees through has moved), a depth step drawn as a last-resort stretch, then a background-biased fill. The world from the held eye, the hands from the fresh eye by the hands mask (the held eye's own hands are dropped). Submitted with the fresh locate generation's pose. |

Design choices that differ from Dishonored, and why:

- **No body band** (rotation-only vectors near the camera). Dishonored's depth contains the
  arms; BS1's world depth does not (the arms are the separate foreground pass, covered by
  the hands mask), so the band could only ever catch near world geometry, where it is
  wrong. Measured: with the band on, the integration test's vectors were off by 1.26 px
  (a crate 0.3 m away); off, 0.001 px. Lever: `dlss body <m>`.
- **Depth from the D24 buffer**, not a colour target's alpha (D3D11, not D3D9).
- **The game's camera, not the XR pose model**, for both the vectors and the warp: walking,
  stick turning and the head are all in it exactly.

## BioShock facts used (and how sure)

From `ENGINE_NOTES` "D3D11 frame map": the scene depth is `D24_UNORM_S8_UINT` at the
backbuffer size with an R24X8 SRV view (so a typeless resource); the world renders through
`DrawIndexed` only; the scene DSV stays bound through the tonemap and HUD. From the BioShock
VR DLSS/DLAA community fork (MIT, `temporal_guides.cpp`, IDA on the same exe): a standard D3D
projection, 10 uu near plane, 65536 uu far in gameplay. From `camera.cpp` session 15/16: the
foreground pass renders through the WORLD lens (`vrfgfov` match, default on).

**Not known, and logged for the first run:** whether BS1's foreground pass clears depth
(the design handles both - with a clear, the hands get their own depth and the mask; without
one, the world pass already contains them and there is no mask). The 5 s log line prints the
pass structure of the last image, e.g. `100W 17F` = a 100-draw world pass and a 17-draw
foreground pass were copied.

## Verification (offline, this machine)

`.\tools\gpu-test.ps1` - seven suites, 121 checks, all pass (86 at the first handoff; the
second round added the real CalcView pattern, AER, the clamp arithmetic, the split world
pass and the drift resync; the third the probe, the gain, the bands and the soaks; the
fourth the viewmodel walk, the squeezed-range mask and the panel's own-image rule):

| Suite | Checks | Key numbers |
|---|---|---|
| `temporal-gpu` | 26 | Fourth round: the hands mask is exactly the pixels inside the squeezed depth range (945 of 945, 0 wrong; none marked without the range). Third round: the hand-depth probe on a native pair reads 1.00 for hands and world on true depth, 1.12 / 1.00 with the hands 12% too deep (~1,500 hand, ~10,000 world samples). Vectors vs ray-traced truth: head turn p95 0.011 px; walk + turn + roll p95 0.016 px (near object 0.03 px); the real D24 path p95 0.019 px. Controls: rotation-only under translation 11.4 px, wrong near plane 5.3 px. Audit: vectors 0.010 vs no motion 0.112, depth scale 1 the minimum. Foreground mask and depth exact. TAA 0.031 vs raw 0.073 (control: zero vectors 0.061). |
| `dlss-host` | 11 | The 32-bit client starts the x64 helper and NGX in ~1.5 s. DLAA with the production vectors 0.0255 vs raw 0.0441 (none 0.0432, negated 0.0941); jitter sign -1 confirmed; SR 512x480 -> 768x720 0.0289 vs bilinear 0.0452; reset; clean stop. DLAA ~0.5 ms/eye at 384x360, SR ~2-3 ms at 576x540 output. |
| `afw-gpu` | 18 | Fourth round: a textured viewmodel carried in the head's frame through one real frame of walking (1.5 uu, 0.4 deg, 640x600): hand region 0.0155 with the hands from the fresh eye vs 0.114 with the old code (the control), the world no worse (0.0300 vs 0.0332), and with no masks bound the old code exactly. Rebuilt held eye vs truth 0.0035 vs the compositor's rotation-only 0.0289; identity exact; every part beats its control (held only, fresh only, stale test off). Third round: the mesh in 6006-vertex bands equals one draw exactly; a 12% hand depth error is undone by gain 1.12 (hand region 0.0325, as with true depth; 0.0361 without). ~1.3-2.3 ms per rebuild at 2064x2208. |
| `afw-soak` | 1 | Third round: 30 s of rebuilds at 2750x2850 (~1,900), 32-bit, this machine's NVIDIA driver: no crash, no failed rebuild. |
| `temporal-integration` | 47 | Fourth round: with the F10 panel up the held eye re-shows its own last image every present with its pose kept (4 of 4, never the fresh eye's image), and the first rebuild after the panel does not source the panel-time image; the hand in the rebuilt eye 0.018 with the mask in the store. Second round: `alt_eye.cpp` too, driven with four CalcView calls per frame (three consumers, then the render view in the build): AFW and AER carry one eye per frame on every call, strictly L R L R, every AFW image named right; AER tags one eye per frame, opens every pair LEFT (also after a non-stereo frame), renders the right frame from the left's camera at all four call sites (0.0000 uu) and from one head sample; the clamp gives 0 on right frames for both receivers, pays the bank next frame, loses no time over 26 frames, and drops the banks when a load swaps the receivers; under AFW it never holds. A world pass split by a shadow pass (no clear) stays the world, the hands mask exact. First round: `temporal.cpp` itself, with core and the engine stubbed: the world pass picked over the hands pass (100W 17F), every image paired with its camera (integrated vectors 0.001 px), DLAA 0.024 vs raw 0.037, SR 0.037 vs bilinear 0.044, TAA 0.028 vs raw 0.060 with the helper stopped, every AFW image named with its own eye, held eye 0.0016 vs rotation-only 0.0092, the hand in the rebuilt eye 0.011, F10 open leaves the held eye alone, AFW + DLAA together, off restores everything and stops the helper. |

| `xr-seams` | 17 | Second round: AER pairs through core's SR pair path show each eye its own image (dark left 38, bright right 217); one stale tag swaps the eyes for good with the drift resync off (the control) and they come back with it on. First round: the production `openxr_runtime.cpp` on the simulated Quest 3 runtime (no game; HUD, input and overlay stubbed), judged by the simulator's own per-eye compositor captures: SR images go through the processor eye by eye (the compositor shows each eye's processed image), an output size rebuilds the eye swapchains at 960x960 and a declined image is stretched, a size the runtime refuses (20000x20000) falls back with the session alive, AlternateEye with the eye source and held rebuild shows one fresh and one rebuilt eye every frame and opens the HUD gate. |

Three real bugs these caught before any game run: the present counter advances before the
eye processor runs (the depth would have been discarded every frame); a joinable worker
thread left in a global (`std::terminate` at game exit); and a super-resolution size a
runtime refuses would have failed swapchain creation every frame (now dropped, with a log
line).

## Headset questions, in order

1. **DLAA on, standing and turning:** are edges stable, and is the image as sharp as off?
   Log: `[dlss] dlss: eye 0 ready ... (DLAA)`, then every 5 s `[temporal] 5 s: ... dlss N`
   with N close to the eye frame count, and `no depth 0`.
2. **The pass structure line:** `depth passes of the last image: ...`, now with `|` for a
   clear of the world's depth. `W ... | ... F` = a separate foreground pass (the hands mask).
   `W w w ...` with no `|` = the foreground is inside the world pass (no mask; say if the
   hands smear while walking with DLAA).
3. ~~The vector audit line~~ - answered by the first run (vectors 0.0071 vs no motion 0.0171,
   depth scale 1 lowest). Off by default now.
4. **Walking with DLAA:** trails behind edges? (vectors). On the hands? (mask).
5. **Super Resolution Quality** at a lowered game resolution: sharpness vs the same
   resolution with DLSS off, and the frame rate.
6. **TAA:** shimmer on fine detail vs off, and any trailing while walking (`dlss taaweight
   0.2` trades smoothness for less lag).
7. **Stereo method: Alternate eye (AER)** first (the simpler of the two): does it look like
   BioShock Remastered VR - steady stereo, the near world fused while walking? Log every 5 s:
   `[stereo] 5 s (alternate eye (AER)): frames L n R n` with L = R, `world held` = R and
   `paid` = L, `render view moved within a pair` near 0 standing and small walking, and once
   `[aer] frame-delta hook live`. Levers: `aer clamp off`, `aer lock off`.
8. **Stereo method: Alternate frame warp (AFW):** does the world hold still in both eyes while
   turning and walking, and are the hands steady? Log: `[stereo] 5 s (alternate frame warp
   (AFW)): frames L n R n` with L = R and `mono 0` in gameplay, and the temporal line's `afw
   rebuilds N (with the held eye M)` with N close to the eye frames. `afw tint on` colours the
   rebuilt eye by source (blue = the other eye, green = its hands, magenta = fill). If the game
   crashes again: the crash lines in the log and the dump path, and whether DLAA was also on.
   **Ninth run, in this order:** (a) AFW standing still: are the arms and hands single,
   stereo and steady (the fg-camera fix)? Then the F10 panel up: the arms normal there too?
   (b) F10 > Display > Resolution > Apply now (live) at 2048x2048: `[resize] CONFIRMED`? Then
   back to 2750x2850, then the Super Resolution status's "Render at that size now". (c) The
   capture standing still, for the probe: hands measured/predicted should now read ~1.0.
   **(The eighth run's list, kept:)** (a) AFW standing still looking at the hands: are the arms
   whole and steady, no jitter between the eyes? F10 Debug "AFW: hand depth gain" 1.00 brings
   the seventh-run jitter back (the A/B); 0.65 is the capture's measurement. (b) Walking and
   turning with the sticks: hands single and steady ("AFW: hands from the fresh eye" off = the
   fourth-run ghost)? (c) The F10 panel: say exactly what is wrong with it up - the hands
   doubling, the panel itself doubling, flicker between the eyes - the held eye re-shows its
   own last image while it is up. (d) Super Resolution: at 2750x2850 it now runs as DLAA; to
   see SR itself, F10 > Display > Anti-aliasing status > **Render at that size now** (1834x1900
   for Quality) - the log should say `[resize] CONFIRMED` within 3 s and the eyes keep their
   size; then compare sharpness and frame rate. If it says NOT TAKEN, the window was clamped
   (say what size it reports). (e) A new AFW capture standing still if anything
   about the hands is still off: `tools\afw-replay.ps1 -Tint`, the pictures and the
   hand-region score (the probe line reads the native frames, so it stays 0.645 whatever the
   gain; it changes only if the hands' rendering changes).

## Audit against Dishonored's AFW and BRVR's AER (2026-10-10, second round)

Sources: Dishonored VR mod `output/AFW_RENDERING_GUIDE.md` (sections 4, 10, 11), BRVR
`Camera/CameraHook.cpp` (eye FIFO, PAIR LOCK, PHASE 10a delta clamp, delta scan,
CameraHook_NextEye), `docs/modules/render.md` and `docs/INVARIANTS.md` (threading and frames).

| Rule | Source | Here |
|---|---|---|
| The eye is decided on the game thread by strict alternation, never inferred from timing | both | Yes - `alt_eye`, once per frame at the render view |
| Only the render view counts (BRVR's leader site; Dishonored's per-draw push) | both | Yes - `scenedraw::in_gameplay_build` (was: every CalcView call - the flicker) |
| One tag per frame, pushed before that frame's Present, popped one per Present | both | Yes - AER into core's SR ring, AFW into temporal's FIFO |
| A non-stereo frame restarts the sequence on the left | Dishonored 4.1 | Yes |
| Eye-queue resync after an underrun, and after 8 pops in a row with a stale tag queued | BRVR NextEye | Yes - AFW's FIFO; AER via core's opt-in `set_sr_drift_resync` (sim-tested with a control) |
| AER: the right frame renders from the left frame's camera | BRVR PairLockCamera | Yes - per CalcView call site |
| AER: one HMD sample per pair | BRVR (latched on eye 0) | Yes - `latch_head` |
| AER: the world advances once per pair, each receiver with its own carry, carries reset when a load replaces the receivers, an implausible carry (> 0.1 s) dropped | BRVR DeltaClamp=2 | Yes - same function (re-derived on this exe: unique signature, `ret 8`), the arithmetic host-tested |
| AER: both eyes submitted together with the pose they were rendered from | BRVR XR_SubmitPair + latched pose | Yes - core's SR pair pacing (left holds the frame, right closes it, one locate, lag-1 attribution as SR) |
| AFW: the delta clamp OFF (every frame is shown on its own) | Dishonored 4.1 | Yes - `clamp_delta` passes through unless AER |
| AFW: every present its own XR frame, pair pacing off | Dishonored 4.3 | Yes - core's AlternateEye path |
| AFW: depth, camera and image kept per image; the held rebuild refuses unless the fresh record is this present's | Dishonored 4.4-4.5, 10 | Yes - `g_freshValid` set by this present's processor, cleared by the rebuild |
| AFW: an epoch drops records when the meaning changes; fail soft everywhere; never wait on a fence | Dishonored 10 | Yes |
| AFW: a held-eye body-yaw fallback when the rebuild refuses | Dishonored 10 | **No** - the refusal copies the held image (rotation-only). Rebuild refusals were 0 in the run |
| AFW: the exact locate generation of each image (an 8-deep pose history keyed by generation) | Dishonored 4.2 | **No** - core's fixed lag 1, which is right only if each frame's CalcView reads the locate of the previous present. Symptom if wrong: the hands grow and shrink on fast head turns |
| AFW: seed maps at half size + a per-pixel fixed-point refinement | Dishonored 6 | **Different** - full-size depth meshes from both sources (0.0035 vs rotation-only 0.0289 on the host; ~2 ms estimated at 2750x2850, no GPU timer yet) |
| AFW: a GPU timestamp on the beat line | Dishonored 10 | **No** |
| AER: the controller (aim and hands) latched per pair | BRVR motion aim | **No** - the right frame's hands use a controller sample one frame newer than the left's |
| The menu/starved path: a mono quad | BRVR | Yes - core's existing untagged-present path |

## Files

`src/core/gfx/dlss_ipc.h`, `dlss_client.{h,cpp}` (transport), `src/tools/dlss_host/` (helper,
`NOTICE.md`), `src/game/bioshock1r/alt_eye.{h,cpp}` (the stereo methods: eye sequencing, AER's
pair lock, head latch and frame-delta clamp), `temporal.{h,cpp}` (glue), `temporal_gpu.{h,cpp}` (guides),
`afw_gpu.{h,cpp}` (warp), core seams in `openxr_runtime.{h,cpp}`, `frame_inspector.{h,cpp}`,
`blit.{h,cpp}`. Tools: `tools\fetch-ngx.ps1` (pinned NGX SDK v310.7.0, gitignored),
`tools\build-dlss-host.ps1`, `tools\gpu-test.ps1`, `tools\install.ps1` (copies
`bvr_dlss\`).

## 2026-10-11 polish item 1: enable the alternate-eye foreground view-sync

AER/AFW method changes now enable the prologue-checked foreground ctor hook and the
view-sync together, logging `[fgnode] view-sync ON for <method>`. Returning to both-eyes
restores the previous sync preference; `vrfgnode sync off` remains a diagnostic control.
The five-second temporal line now reports `fg sync subs/misses`. The ctor and the offline
pretend game share the camera-substitution function. **Correction after run ten:** this
first fixture put the ctor before the render CalcView and fed only the hands from it;
that did not match the engine. The numerical result below is historical, not proof
that the shipped game's cameras were correct. In `temporal-integration`, 41 substitutions
put it within 0.00001 uu of this frame's eye; the previous-frame control is 7.899 uu away.
Disabled sync and a missing camera preserve the engine arguments. All 49 integration checks
pass, as do the full GPU runner, host tests, lint and Win32 Release build. No game launch;
the headset still needs to establish solid arms and a native-pair hand disparity near 1.0.

## 2026-10-11 polish item 2: pin the live SetRes door

The live resize now requires the ninth run's render-device vtable, slot-61 entry and
prologue together, with the RVAs, signature and viewport fields in `patterns.h` and their
derivation in ENGINE_NOTES. Explicit [Bioshock1R] RVA overrides retain the byte check;
invalid values fail closed and a mismatch reports both expected and actual targets.
`gpu-test.ps1 resize` runs the production resize.cpp against a synthetic x86 viewport:
10 checks pass for identity/prologue failures before mutation, re-resolving a lost viewport,
the exact calling convention and counter restoration, backbuffer confirmation, invalid
overrides and engine refusal. All eight GPU suites, host tests, lint and Win32 Release pass.
The existing engine SaveConfig already saved the ninth run's final 1834x1900 windowed size
in Bioshock.ini (StartupFullscreen=False), verified against the v0.8.3-239 log, so no second
INI writer is added. No game or standalone simulator was launched.

## 2026-10-11 polish item 3: Display controls and live mode-driven resolution

Display now starts with Rendering method, Resolution, then Upscaling/anti-aliasing.
Stereo/AFW and the on/Mode controls are Basic; Model is Advanced; AER, TAA and the
AFW diagnostics are Debug. Saved quality values and seam commands retain their meaning.
The total-pixels slider scales both axes by the square root, with **100% = 2750x2850**
per the tester's 2026-10-11 correction (replacing the plan's runtime-recommendation
baseline). Applying it chooses the output; the game-thread scheduler resizes the render
for each SR ratio and restores output size on DLAA/off. It keeps the latest request
while SetRes is busy and does not retry a refusal every frame. Ultra Performance rounds
up to an even 918x950 at that baseline so NGX's 3x limit cannot shrink the chosen output.
Status reports render -> output and mode; the fixed list, INI button and manual SR resize
buttons are gone. The production F10 view passes 373 interaction/layout checks at both
760x1000 and 570x640, with rendered previews inspected; display-model passes 7 checks.
Temporal-integration passes 62 checks, including all five ratios, busy/latest intent,
refusal, SR-off restoration, DLAA, invalid quality and exact-output rounding. All nine
GPU suites, both host suites, lint and Win32 Release pass; the final rounding change
was separately rebuilt and rerun through integration. Panel-independent processing
follows in item 4. No game or standalone simulator was launched.


## 2026-10-11 polish item 4: F10 outside the temporal images

BS1 opts into a native ImGui canvas at OpenXR's recommended eye resolution. A separate
premultiplied-alpha quad is composed last, head-locked at HUD depth; the controller's
origin and direction intersect that same plane in head space. The desktop mirror draws
the same panel after eye capture/processing, scaled to its own backbuffer. BS2/Infinite
keep the old default. DLSS, TAA and AFW no longer inspect panel visibility or invalidate
history when F10 opens. Offline: overlay-layer passes 12 checks for clean eye inputs,
native texture reuse, alpha, preservation of bound targets, mirror scaling and finite-depth
ray mapping. The production XR seam suite passes 21 checks: both compositor captures show
the overlay; its 2064x2208 texture survives a 640 -> 960 output resize; all 25 observed
panel presents process and rebuild; closing it removes its pixels. Integration passes
62 checks, including 60 TAA eye frames with F10 up, every one of four panel-up AFW
rebuilds, retained history on closing, and DLAA plus all 30 AFW rebuilds during helper
startup/steady operation. All ten GPU suites (including the two driver soaks), host tests,
lint and Win32 Release pass. Native sharpness, finite-depth cursor alignment and headset
comfort remain tenth-run checks. No game or standalone simulator was launched.


## 2026-10-11 polish item 6: six presents, replay then delete

The default AFW capture is six presents from both F10 and `afw dump`; explicit counts
remain available. `tools/afw-replay.ps1 -Delete` removes only the named capture folder
after successful replay. The path is resolved before entering the build directory and
verified again before literal-path removal; roots, changed targets and reparse paths are
refused. Replay previously returned zero even when it skipped a present or failed to
write pictures. It now returns failure unless every present rebuilds and its requested
picture writes succeed, so those failures cannot trigger deletion. Four synthetic
capture checks pass: ordinary replay preserves captures/pictures; successful `-Delete`
removes them; missing depth and a picture-output failure preserve the capture. Synthetic
fixtures were cleaned after these checks. All ten GPU suites, both host suites, lint and
Win32 Release pass. No gameplay capture or game launch was needed.


## 2026-10-11 polish item 5: refine AFW silhouettes per output pixel

AFW now rasterizes half-resolution source-UV seeds, then solves each output pixel against
source depth with three fixed-point corrections. A raw-depth neighbour retry recovers a
near surface between mesh vertices; the final residual must stay on the same depth
footprint, preventing a one-texel rail from widening into two. Stale/hidden tests use the
refined point, and the under-six-depth-texel near-miss rule applies to world surfaces only.
The existing BS1 world-unit tolerances are retained. `afw refine off` restores the original
full-resolution mesh compose exactly; refinement defaults on. Captures record the choice,
replay follows it (old captures default to the mesh control), and `-Refine 0/1` overrides it.
The replay script's build-staleness variable no longer shadows its `-Stale` argument, so
repeated A/B runs keep the same visibility control. Four synthetic replay checks cover old
and new metadata, both overrides, cached stale settings and successful deletion.
`afw-gpu` passes 24 checks: total silhouette error 63.685 versus the mesh's 667.073 (9.5%),
diagonal-edge error 45.217 versus 188.566 (24.0%), and the one-depth-texel rail retains all
224 truth pixels versus zero with the mesh. Switching refinement off restores the exact
control image. The 2064x2208 GPU check measured 1.63 ms alone and 2.16 ms in the full run,
below the 4 ms gate; this synthetic timing is not a gameplay performance claim. All ten
GPU suites, both host suites, lint and Win32 Release pass. No game or standalone simulator
was launched. The tenth run must still establish real-scene silhouettes and arm stability.


## The tenth headset run (2026-10-11, 13:02): whole-scene flicker reproduced

The tester reported "massive flickering on everything back and forth" on the verified
installed banner **v0.8.3-248-g419b304**. DLAA and AFW continued while F10 was open; the
steady-gameplay log had zero missing depth/cameras and no AFW fallbacks. The auto output
was the runtime's 2688x2880 (the 100% slider preset remains 2750x2850). Continuous
processing and the passing offline suites did not establish correct eye identity.

The six-present capture `afw-20261011-130341` was replayed with refinement both on and
off. Across five scored presents the refined/native-next-frame luminance error was
0.0183 (hands 0.0395); mesh was 0.0185 (hands 0.0401). The refined replay reproduced the
in-game rebuild. Both paths showed the mismatch, so toggling refinement would not cure
this regression. Native-frame/depth reprojection also contradicted the recorded eye
motion: in pair 4->5, reversing the tagged translation gave hand error 0.02585 versus
0.06500 using the recorded direction. Capture stalls produced two sync misses, so not
every pair is a clean motion reference. No hand gain was fitted from these images.

**Cause:** CalcView precedes the player-scene constructor, contrary to the old notes and
test fixture. CalcView records the current eye and advances the next eye. The ctor then
read that next-eye value and substituted the other eye's cached camera into the entire
scene. Headless IDA verified the order and node identity in this exact game binary;
ENGINE_NOTES records the call sites and MD5. This was a camera-selection regression
introduced by enabling the substitution, not evidence that the mesh/refine math broke.

**Change:** a separate committed-eye latch supplies the ctor. Every root build clears
it before CalcView, so a build with no camera commit cannot reuse an old gameplay eye.
The integration fixture now commits CalcView before constructing the scene and draws
both world and hands from the ctor camera. On the unfixed code this gives 59 passes and
three failures (7.89934 uu wrong camera, world rebuild 0.04077, hand error 0.1505).
A scripted build without CalcView and AER after its eye advance are also covered.

The corrected feature build passes all ten GPU suites, including 64 integration
checks and 21 XR seam checks, plus both host suites, lint and Win32 Release. Camera
error is 0.00001 uu; world rebuild error falls to 0.00126 and hand error to 0.0196.
AER has zero ctor/tag mismatches; the no-CalcView build performs no camera lookup.
The replacement merged build and installation are recorded in the handoff. Headset
acceptance remains pending; the next launch asks only whether the whole-scene flicker
has stopped. Earlier hand disparity/arm transparency still need assessment after that
regression is gone. The analyzed capture and both replay-image directories are deleted
after recording these findings; only logs/numbers remain, never game images in Git.


Replacement local test build: **v0.8.3-251-g66a102f**, merge `66a102f` carrying fix `0e5f4a9`
and the existing hands work. All ten merged GPU suites and all six merged host suites,
lint and Release pass. Installed files and archived DLL/PDB hashes match; all seven
complete INIs remain byte-identical. The feature worktree is restored. The eleventh
run's one stability question and exact install/archive details are in the handoff.


### Run eleven - world stability accepted; arm-origin correction (2026-10-11)

The tester reports v0.8.3-251-g66a102f looks almost perfect in the world; the arms
still flicker while the hands do not. The captured build and log were verified.
The render-eye translation was being published as the IK head position, moving the
shoulders by an IPD while controller wrists stayed fixed. FrameContext now removes
the actual applied eye translation, leaving camera metadata untouched; static body
regression: 3600 checks / 600 poses, no failures, old path visibly distinguishable.
All ten GPU suites, three feature host suites, lint and Release pass with this
correction and the foreground edge repair under test. Live arm acceptance is pending.

The requested target is now matching native stereo quality. The captured white
pistol edges are being replayed separately; neither a disparity gain fit nor a
successful still-image test alone establishes that target.


### Run eleven - retain depth-confirmed foreground at failed fresh silhouettes

The refined shader discarded held foreground even when the fresh eye's depth
confirmed the same surface. If the fresh inverse solve missed that curved edge,
composition exposed the bright world behind the pistol. It now retains held pixels
whose projected fresh depth and foreground mask agree; a valid fresh foreground
candidate keeps priority, and depth that sees past a moved object still discards it.

A new curved-cylinder fixture uses 480x450 antialiased color and 320x300 depth,
three radii and five lateral positions. Bright interior failures fall from 3870
to 960 of 95296 tested pixels. The moved-cylinder control leaves 2 ghost pixels
versus 5044 with held-only rendering. All 26 AFW checks pass, including the earlier
one-texel rail and diagonal edge; GPU rebuild remains below the 4 ms test budget.
All ten GPU suites, three host suites, lint and Release pass for the feature tree.

On the captured pistol crop (1180,1200)-(1620,1750), replay mean absolute RGB error
against the next native eye improves on all five pairs. Bright overshoot counts
are 89/387/94/482/477 before and 5/101/58/186/491 after. The last pair is a useful
limit: fewer aggregate errors does not mean every edge is fixed. Next-frame native
is an imperfect reference when capture stalls or the hands move. Further work must
cover residual depth/color boundary errors, motion and the native foreground option;
native-equivalent quality is not claimed.


Foreground checkpoint installed: **v0.8.3-255-g81def2d**, local hands merge 81def2d.
All ten merged GPU suites, seven host suites, lint and Release pass. DLL/PDB hashes
match the symbol archive; all seven complete INIs are byte-identical. The twelfth
headset question is pending; the native-quality target remains open.


After recording run eleven, both analyzed capture folders and all replay pictures
were deleted: afw-20261011-133304 (1,724,092,586 bytes) and afw-20261011-133320
(3,498,526,622 bytes), 5.22 GB total. Logs, numerical findings and scratch research
scripts remain; no game imagery is committed. Use a fresh capture from the corrected
body pose if the twelfth run still shows artifacts.

Further uninstalled experiments: preferring confirmed held foreground before fresh
foreground reduces the still-cylinder bright count to 292, while color-guided depth
sampling gives 300 and adds work. Neither establishes correct motion: depth agreement
cannot detect texture/pose motion along a nearly unchanged surface. The installed
repair retains fresh foreground priority. No such experimental shader is installed.


### 2026-10-11 - Run twelve: stable arms, remaining motion gaps and edge colour

Confirmed installed banner v0.8.3-255-g81def2d at 14:12:03. The tester calls the
result extremely good, with the major remaining defects missing pixels in motion
and an edge colour that fits one eye's background but not the other. Captures
afw-20261011-141555 (walking) and afw-20261011-141636 (nearly stationary) are under
analysis. The stationary first pair measures foreground disparity ratio 1.000 and
world 1.001; the body-origin fix is supported by this capture as well as the report.
Steady intervals continue DLSS/AFW on every present, zero depth/camera/foreground
sync misses or rebuild fallbacks, and about 1.82 ms DLSS evaluation.

A second silhouette rejection defect paired a four-texel depth gather with only
one point-sampled mask. The confirming depth could belong to a hand while the
point mask belonged to background. Pairing every gathered depth with its own mask
eliminates two independently reproduced bright columns (136 and 146 pixels in
the installed shader, zero after the fix). Across 15 curved foreground scenes,
bright interior leaks drop from 960 to 669 of 95296 pixels. The moved-away object
control still has only two ghost pixels versus 5044 when using the old eye alone.
The AFW GPU suite passes 28 checks; measured rebuild cost is 1.759 ms at 2064x2208.
This improves the captures slightly but does not explain their larger walking
gaps. Native-quality acceptance remains open.


The motion correction now publishes each build's room-to-game tracking frame and
uses it to carry the held foreground through walking and stick turns. The world
keeps its original camera, and physical head movement remains relative to the
tracking origin. A separate foreground seed handles cases where the old world
would cover the moved hand's coarse seed. Current-eye depth/mask validation still
removes independently moved hands. The AFW GPU suite passes 35 checks, including
2 remaining holes versus 353/360 in walking/turning controls and zero moved-object
ghosts. Rebuild cost measures 1.985 ms for the regular fixture and 1.916 ms with
foreground transport covering the entire 2064x2208 eye, below the 4 ms budget.
The 725-check host transform suite passes. Native masked repair is the next
investigation; this change does not add native geometry rendering.

## 2026-10-14: hand motion vectors (DLSS / DLAA / TAA)

**The smear.** With DLSS on, the hands' and weapon's textures smeared whenever the
controllers moved. The guides gave every hand pixel the foreground transport above:
the hand treated as standing still in the room, carried only through locomotion
(`temporal.cpp` said so: "Head/controller motion is deliberately not included"). A
hand swung through the air therefore had a vector pointing at where it was NOT last
frame, and DLSS blended the wrong history into it. The bias-current-colour mask only
softens that.

**The fix.** `note_camera` now records each controller's GRIP pose per build, through
the same pose funnel and `xr_pose_to_game` map the hands use, with the hand-back
weight (1 = the controller drives that hand). The guide shader gives a hand pixel the
nearer grip's rigid move between the two images of that eye -
`p_prev = R_prev R_cur^T (p - grip_cur) + grip_prev`, then the previous camera - at full
weight within 0.35 m of the grip, fading to the old transport by 0.60 m (the elbow and
upper arm, which the controller only partly moves). A rigid offset between the grip and
the drawn hand (grip calibration, model trim, the aim/grip orientation split) cancels
out of that product, so no hand calibration enters it. A grip jump over 200 uu (a
recenter, a load) or a missing pose falls back to the old vectors for that hand.

**Also fixed:** in Stereo method "both eyes every frame" the right eye's second pass
noted its camera without the frame context, so the right eye's hands got plain WORLD
vectors (not even the locomotion transport). The second pass now takes the left pass's
tracking frame and grips (same game frame, within 100 ms).

**Not touched:** the accepted AFW reconstruction. AFW takes its foreground camera from
its own path (`afw_gpu`); only the DLSS/TAA guide vectors changed.

**Controls.** F10 Display > Upscaling and anti-aliasing (DLSS), Advanced: **Hand motion
vectors** (default on; off is the old behaviour, for an A/B). Console: `dlss handmv
on|off`, `dlss handr <full m> <zero m>`. The log prints `[temporal] hand motion
vectors on` the first time a hand is carried.

**Verified offline:** `gpu-test temporal-gpu` 38/38 - a grip turned 10 deg yaw and 6 deg
pitch and moved (3, -2, 1) uu gives hand vectors within 0.010 px of the analytic truth
(6.13 px true motion; the room-fixed control is off by 6.13 px), every world vector
bit-identical, the nearer of two grips owns the pixel, and no hand / a hand beyond the
radius / weight 0 reproduce the old vectors exactly. **Not headset-tested.** What to look
for: hold a weapon with DLSS on and sweep it side to side - the texture should stay
sharp; toggle the F10 box to compare. Finger animation, recoil and reload motion are
not in the controller pose and still rely on the bias mask.
