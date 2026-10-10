# BS1: DLSS / DLAA, TAA and the alternate frame warp

Branch `claude/bs1-dlss-afw`, off `staging` `ba63f80` (2026-10-10). The Dishonored VR mod's
process (motion vectors from depth and cameras, its custom TAA, NVIDIA DLSS through an x64
NGX helper, and the alternate frame warp), ported to BioShock 1. **Everything here is OFF by
default**; with both switches off no seam is armed and BS1 renders exactly as on `staging`.

**Not run in the game or the simulator yet** (no launch this session). Everything below is
verified offline on this machine's RTX 4070 Ti SUPER by the GPU suites in
`tools\gpu-tests\`, including the real DLSS helper and the production glue. The headset
questions are at the end.

## What a tester does

1. F10 > Display > **Anti-aliasing and upscaling** > Anti-aliasing: **DLAA (NVIDIA)**.
   The status section under it says `ready on <GPU>` within ~2 s.
2. Then **DLSS Super Resolution** (scale under Advanced): it renders at your game resolution
   and reconstructs a larger eye image. Lower the game resolution (Display > Resolution,
   restart) to turn that into speed.
3. **TAA (any GPU)**: the same guides, no helper, lighter and softer than DLAA.
4. F10 > Advanced > Display > **Stereo rendering** > **Stereo method** (`alt_eye.h`):
   - **Both eyes every frame** - SequentialReentry, the default, unchanged.
   - **Alternate eye (AER)** - BioShock Remastered VR's method: one eye per game frame, the
     two frames of a pair rendered from ONE camera and ONE head sample with the world held
     still on the right frame, both eyes shown together as one headset frame.
   - **Alternate frame warp (AFW)** - one eye per game frame, every frame shown, the other
     eye rebuilt from both eyes' images and depth. Combines with any of 1-3.

Seam words for the same: `dlss dlaa|sr|taa|off|retry|quality <0-4>|model <0-3>|audit on|off|
near <uu>|far <uu>|body <m>|fgbias on|off|taaweight <x>`, `aer on|off|clamp on|off|lock on|off`,
`afw on|off|tint on|off|stale on|off|grid <1-8>`.

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

## How it works

| Stage | Where | What |
|---|---|---|
| Depth | `bioshock1r/temporal.cpp` via `core/gfx/frame_inspector` `set_draw_tap` | The draw tap follows the game's depth binding. Per present interval, the backbuffer-size D24 surface with the most `DrawIndexed` calls is the WORLD pass and is copied before it is cleared or unbound; a later pass on the same surface (after the world's clear) is the FOREGROUND - the arms and the weapon - and is copied beside it. |
| Cameras | `camera.cpp` -> `temporal::note_camera` | CalcView publishes the final camera of every build (after every offset). SequentialReentry images take the latest camera of their eye; AlternateEye images carry the exact build through a FIFO. |
| Guides | `bioshock1r/temporal_gpu` | Depth (+ the foreground's where it drew) -> linear view depth, DLSS depth (reversed), camera motion vectors (previous UV minus current), and the hands mask. The vector audit scores them in-game. |
| DLSS | `core/gfx/dlss_client` + `src/tools/dlss_host` | An x64 helper owns NGX (310.7, preset K by default), one feature and history per eye, textures and fences shared by NT handle. The hands mask is DLSS's "bias current colour" mask. |
| TAA | `bioshock1r/temporal_gpu` `TaaGpu` | Per eye: history fetched along the vectors, clamped to the current 3x3 range in YCoCg, blended at 0.12 (`dlss taaweight`). Hands, no history or off screen = the current image. |
| Output | `core/vr/openxr_runtime` eye-processor seam | The result replaces the backbuffer copy into that eye's swapchain. Super Resolution asks core for larger eye swapchains (`set_eye_output_size`); anything not reconstructed is stretched. |
| Eye sequencing | `bioshock1r/alt_eye` | One eye per FRAME, decided on the game thread at the render view's CalcView and carried by every CalcView call of the frame; a non-stereo frame restarts the sequence on the left. AER tags each frame into core's SequentialReentry ring (`vr::sr_push_eye`) and pairs are submitted together; AFW names each image through temporal's FIFO (BRVR's underrun/drift resync). |
| AER | `bioshock1r/alt_eye` | BRVR's method: the right frame takes the left frame's head sample (`latch_head`) and each call site's left camera (`pair_lock`); the frame-delta hook (patterns `kFrameDeltaRva`) holds the world on the right frame and pays the delta into the next left frame (no game time lost); core's pair pacing submits L+R as one XR frame with one locate's pose; core's SR ring gets BRVR's drift resync (`set_sr_drift_resync`). |
| AFW | `bioshock1r/afw_gpu` via core's held-rebuild and AER eye-source seams | AlternateEye, every present its own XR frame, world NOT held (Dishonored's rule). After the fresh eye, the held eye is rebuilt at the same instant from both eyes' images and depths: a depth-tested mesh per source, a stale test (a point the fresh eye sees through has moved), a depth step drawn as a last-resort stretch, then a background-biased fill. Submitted with the fresh locate generation's pose. |

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

`.\tools\gpu-test.ps1` - five suites, 107 checks, all pass (86 at the first handoff; the
second round added the real CalcView pattern, AER, the clamp arithmetic, the split world
pass and the drift resync):

| Suite | Checks | Key numbers |
|---|---|---|
| `temporal-gpu` | 21 | Vectors vs ray-traced truth: head turn p95 0.011 px; walk + turn + roll p95 0.016 px (near object 0.03 px); the real D24 path p95 0.019 px. Controls: rotation-only under translation 11.4 px, wrong near plane 5.3 px. Audit: vectors 0.010 vs no motion 0.112, depth scale 1 the minimum. Foreground mask and depth exact. TAA 0.031 vs raw 0.073 (control: zero vectors 0.061). |
| `dlss-host` | 11 | The 32-bit client starts the x64 helper and NGX in ~1.5 s. DLAA with the production vectors 0.0255 vs raw 0.0441 (none 0.0432, negated 0.0941); jitter sign -1 confirmed; SR 512x480 -> 768x720 0.0289 vs bilinear 0.0452; reset; clean stop. DLAA ~0.5 ms/eye at 384x360, SR ~2-3 ms at 576x540 output. |
| `afw-gpu` | 13 | Rebuilt held eye vs truth 0.0035 vs the compositor's rotation-only 0.0289; identity exact; every part beats its control (held only, fresh only, stale test off). 1.26 ms per rebuild at 2064x2208. |
| `temporal-integration` | 45 | Second round: `alt_eye.cpp` too, driven with four CalcView calls per frame (three consumers, then the render view in the build): AFW and AER carry one eye per frame on every call, strictly L R L R, every AFW image named right; AER tags one eye per frame, opens every pair LEFT (also after a non-stereo frame), renders the right frame from the left's camera at all four call sites (0.0000 uu) and from one head sample; the clamp gives 0 on right frames for both receivers, pays the bank next frame, loses no time over 26 frames, and drops the banks when a load swaps the receivers; under AFW it never holds. A world pass split by a shadow pass (no clear) stays the world, the hands mask exact. First round: `temporal.cpp` itself, with core and the engine stubbed: the world pass picked over the hands pass (100W 17F), every image paired with its camera (integrated vectors 0.001 px), DLAA 0.024 vs raw 0.037, SR 0.037 vs bilinear 0.044, TAA 0.028 vs raw 0.060 with the helper stopped, every AFW image named with its own eye, held eye 0.0016 vs rotation-only 0.0092, the hand in the rebuilt eye 0.011, F10 open leaves the held eye alone, AFW + DLAA together, off restores everything and stops the helper. |

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
   rebuilt eye by source (blue = the other eye, magenta = fill). If the game crashes again:
   the crash lines in the log and the dump path, and whether DLAA was also on.

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
