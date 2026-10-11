# BS1 AFW + DLSS: handoff for the next session (2026-10-10)

Read this first, then `bioshock1/TEMPORAL.md` (design, every headset run, the audit) and the
top of `docs/STATUS.md`.

## Where things are

- Worktree `C:\dev\bioshock-trilogy-vr-afw`. Feature branch `claude/bs1-dlss-afw` (off `staging`
  `ba63f80`): the x64 DLSS helper and core seams, DLSS/DLAA/TAA, AER and AFW (alt_eye.cpp), all
  AFW rounds. Local test branch `claude/bs1-test-hands-temporal` = the feature branch + PR 91's
  hands (with PR 88's arm IK) + `claude/bs1-weapon-attach-fx`; the installed build is from it
  (`v0.8.3-221-ge71efe5`). After a change: commit on the feature branch, merge it into the test
  branch, build `-Release`, run the suites, install.
- Offline verification: `.\tools\gpu-test.ps1` (7 suites, 119 checks; afw-soak and
  afw-soak-imgui take 30 s each), `.\tools\host-test.ps1`, `.\tools\lint.ps1`.

## The fourth headset run (20:48), measured

- **The viewmodel is drawn into viewport depth [0, 0.010]** next to the world's [0, 1] (the draw
  tap's census: ~500-900 of every sampled frame's draws; also a [0.990, 1.000] range, the sky).
  The squeeze re-expansion is on and the ghost when standing still is GONE.
- **No crash**, with the F10 panel opened and closed five times in AFW: writing the held eye's
  swapchain on every present (round four) looks like the fix. Keep watching.
- AFW ~130-140 fps where SequentialReentry gave 110-115; **AFW + DLSS Balanced ~85 fps** (in
  Dishonored the combination costs about what plain stereo does).

## Open problems, in order

### 1. Hands and weapon lag one frame and ghost while moving with the sticks

Still: fine. Walking (left stick) or turning (right stick): the hands/weapon trail by a frame and
double. Cause (by the design): the held eye is rebuilt mostly from source A, its OWN previous
image, carried forward as world geometry by the camera move. The viewmodel is not world
geometry - it moves with the camera - so A's copy of it lands where a world object would and
the fresh eye's (B) copy where it really is. Standing still the two agree.

Dishonored solved exactly this: **the world comes from the held eye, the hands from the fresh
eye**, selected by a foreground mask (Dishonored VR mod `output/AFW_RENDERING_GUIDE.md` 7.1,
9.4 "the DRAWN mask", 9.7, 9.9; also 4.6 body yaw and 4.7 controller grips per image). BS1 now
has the mask for free: **a pixel whose raw device depth is below the squeezed range's top
(0.010) is viewmodel**. Build a hands mask in `temporal_gpu` from the raw depth (before
re-expansion), carry it per eye in the AFW store, and in `afw_gpu` (a) drop A's viewmodel texels
entirely, (b) take viewmodel pixels only from B, (c) fill the world revealed behind A's old hands
from B's world, then the neighbourhood search. Also use the mask for DLSS (the bias-current-colour
mask, `in.bias`, today only fed by a separate fg pass that BS1 does not have) and TAA. Test it in
`afw-gpu-tests` with a viewmodel that moves WITH the camera between A and B (today's test hand
moves independently), the control being the current code.

### 2. With the F10 panel up, the hands ghost (the inverse of before)

Round four writes the fresh eye's image into the held eye while the panel is up (so its
swapchain never pauses - the crash fix). Each eye then alternates between its own image and the
other eye's: the old flicker. Keep writing every present, but write the held eye's OWN last
image and keep its pose (`held_rebuild` returns 2): the compositor's rotation-only reprojection,
as before round four. (`temporal.cpp` `held_rebuild`, the `overlay::visible()` branch; the
integration test's panel case asserts the current behaviour - update it.)

### 3. DLSS Super Resolution upscales BEYOND native instead of rendering below it

The log: `2750x2850 -> 4124x4274` (Quality) and `-> 4300x4456` (Balanced) - the eye swapchains
grow to 1.5x/1.72x per axis (the tester's VD overlay showed ~160%), the game still renders at
2750x2850, and every later stage (DLSS, AFW's rebuild, the compositor) pays for the larger
image. That is the round-one design (`set_eye_output_size`, TEMPORAL.md "Output"), and it is
wrong for players. **Dishonored's design is the reference:** the OUTPUT is the headset
resolution the player chose; the GAME's render size becomes output / ratio through a live resize
(`src/core/gfx/dlss.h` header comment, `src/game/dishonored/viewport_resize.cpp`, `dlss output`).
For BS1, find how the render size is set and whether it can change live (the F10 Display tab
says "Resolution changes need a restart"; the mod's VR preset sizes the game's swapchain - look
at core's `ResizeBuffers` handling, `recommended_eye_size`, the BS1 VR preset and the game ini;
ENGINE_NOTES and the UnrealScript corpus for a live `setres`). If no live path exists, the honest
version is "SR sets the game resolution on the next start". DLAA stays native-in, native-out.

### 4. DLSS cost

At the oversized output: evaluate 3.65 ms GPU per eye plus a 1.35 ms helper round trip
(`[temporal] 5 s ... DLSS ready ... evaluate GPU x/y ms, round trip x/y ms`). Problem 3 should
cut the evaluate cost hard. Then look at the round trip: today the present thread waits for the
helper (pipe + fence) for every eye image; Dishonored's guide says every cross-API fence is
polled, never waited on (10, "Never stall a VR frame"). Measure first, after problem 3.
