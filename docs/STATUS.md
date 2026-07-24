# Project status

> Handoff file. Rewrite "Current state" and "Next steps" every session; append to the session log.

## Current state (2026-07-24, session 8)

**The 1t LOAD HAZARD is CLOSED and stereo is now one sticky toggle - the last sharp
edge from session 7 is gone (all flat-verified).** Single-threading is STRUCTURAL:
`reentry 1t on` MinHooks the flush-point (0x61D260, every byte re-confirmed by a
capstone disk disasm) and reproduces its decoded INLINE branch in the detour (copy
args to the render manager, stamp mode, call the drain through its guarded target),
leaving the hw-thread numerator global UNTOUCHED so its load-path consumers see the
true core count. **Load-crossing soak PASSED** with `1t` + `stereo` both armed: an
in-game save load, a quit-to-main-menu teardown, a new-game load, AND the bathysphere
DESCENT into Rapture (a real multi-map streaming transition) - zero crashes, zero new
dumps, guardskips 0 throughout, stereo re-engaging on arrival. The session-7 poke
crashed a loader on the first of those; the hook survives all four. Because of that,
`1t on` (hook mode) no longer refuses at the menu, and the off-before-load warnings
now live only on the legacy `reentry 1tpoke` (the poke, kept as a fallback).

**Everything folds into `vrstereo on|off`** - a top-level seam command, `reentry
vrstereo ...`, and an overlay "VR stereo" checkbox - that sequences structural 1t +
VR camera mode + stereo (reversing on off) and is STICKY across loads. Flat-proven:
one `vrstereo on` at the MAIN MENU armed all three (`VRSTEREO READY`), a CONTINUE-load
carried straight into Rapture with stereo doubling live and NO re-arm, and `vrstereo
off` restored mode=MT / build==presents / drain back on the render thread. Perf in the
Rapture arrival scene (heavier indoor geometry): ~81 pairs/s = 162 presents/s sustained
(eye-offset img-diff 6.5 vs 0.28 phase-consistent floor); lighthouse spawn 225 pairs/s
- both clear M4's 72-pairs/s bar.

**Session 7 (still current where not superseded):** M4 rung 2 had its FIRST IN-HEADSET
TEST and PASSED - real per-eye parallax at full rate, depth correct, world scale good
("pretty good and working as intended"). Head-motion eye weirdness was fixed by
xr-frame-per-pair pacing and USER-VERIFIED ("a looot better... comfortable now"). A
SMALL head-motion bobbing is PARKED to M9 by the user's choice. HUD-in-both-eyes still
unobserved (spawns have no HUD) - open M9 item.

**The session-6 blocker dissolved under forensics.** The minidump work (hand-parsed
MiniDumpNormal parser + capstone drain-head disasm, scratchpad) proved the
drain+0x33 null-deref is the render PUMP thread entering the drain with the
submitted-frame slot `[this+0xC]` NULL (fault addr 0x40 = its +0x40 viewport
member; registers cross-check in all specimens) - and that ALL three evening
crashes were THREADED-mode processes. The bigger surprise: **`-onethread` is not
parsed by the remaster at all** (string absent from the image; the pump thread ran
with the arg on the command line; the pump globals session 6 hexdumped are zero at
the menu in EVERY mode - they are created at first world load). The "onethread
substrate" never existed.

**The real single-threaded switch was found and shipped.** Full decode of the
flush-point decision chain (0x61D260, ENGINE_NOTES): every veto selects the INLINE
drain; the ONLY route to the threaded pump hand-off is
`[kNumHwThreadsRva]/[kThreadDivisorRva] > 1` (live 12/1 - a tight 10-reference
pair, written once at startup, consumed by seven inlined copies of the same test).
`reentry 1t on` arms the drain empty-slot guard, then pokes the numerator to 1:
every scene flush drains INLINE on the game thread. Live-verified: heartbeat
`mode=1T`, beatTid == calcTid, drain caller ret 0x61D367 (the inline call site),
submit stops firing in mono, pump sleeps forever. Mono cost ~20% (413 vs 530
presents/s) - irrelevant against the 144/s VR needs.

**Stereo on that substrate, flat-verified end to end (all session 7):**
`reentry 1t on` + `reentry stereo on` = every build doubled L/R at 168-471 pairs/s
(scene-dependent; ~225 typical in the save spawn), presents EXACTLY 2x builds, all
on the game thread, guardskips 0, no waits that can deadlock. Eye-offset render
diff 2.03 mean vs 0.33 floor; consecutive captures phase-consistent (0.43). **5-min
stationary soak + ~6.5 min of synthetic PLAY (13 clean WASD/mouse cycles navigating
up the lighthouse stairs; the second pass was cut short by the session wrap, not by
any defect) - zero faults, zero new dumps, zero watchdog events** (previous best
under threaded stereo: 16 s-3.5 min to deadlock/crash). User's call 2026-07-24: no
further flat passes needed - "everything looks good and the game was running
smooth".

**Defenses shipped so the threaded trap cannot recur silently:** `reentry stereo
on` REFUSES a threaded substrate (`stereo force` for experiments);
`render_is_threaded()` mirrors the engine's own decision chain; heartbeat/status/
overlay carry a `mode=MT|1T` tag; the drain hook (auto-installed by `1t on` and
`stereo on`) skips any empty-slot drain - the crash state - with a `guardskips`
counter. Watchdog stays detect-only.

**Known expected imperfections for the first headset test** (not failures):
per-present xrWaitFrame pacing halves game tick under a headset (xr-frame-per-pair
queued as polish), HUD renders in both eyes (M9 tie-in), IPD/world-scale not yet
calibrated.

## Previous state (2026-07-24, session 6)

**DR-5 is DONE - the engine renders a second full frame per game tick under our control,
flat-verified end to end.** The session-5 submit hypothesis was half right: hooking the
submit (0x585AC0) worked perfectly (gameplay telemetry: exactly 1 submit per present,
single call site, loc/rot == CalcView's camera), but DOUBLE-calling it is ABSORBED -
thousands of doubled submits, zero faults, zero extra presents, the yawed camera never
rendered. The view data is baked into the command queue during the game-thread BUILD
(consistent with DR-3's per-draw VS b0 finding). Following the submit's live caller RVA
into the disk image (capstone, installed this session) found the real seam: the **scene
BUILD root at RVA 0x4CCE70** (aligned-stack `push ebx` prologue - the 55-8B-EC scan hits
a decoy SEH function at 0x4CCD20; the boundary is a CC-padding run). CalcView runs
exactly ONCE inside every build call (live: calcview-in == build/s == presents/s).

**Double-calling the BUILD is the SequentialReentry primitive, proven:** pulse = second
call does real work (~2 ms vs ~60 us pass-through), re-submits, lands an extra present
during the call, CalcView re-enters and takes the second-pass yaw. Continuous (`reentry
on`, yaw 30): build 225/s all doubled, submit == presents == 450/s (TWO engine-paced
presents per game frame), game tick halves gracefully, and captures show the world yawed
30 degrees (img-diff 7.8 mean vs 0.33 noise floor). `off` recovers instantly. Stability:
~3.5 min continuous clean, then ONE hang (~124k doubled frames; struck during a focus
cycle - kill + relaunch, TESTING warning updated; hardening folds into the per-eye work).
DR-5 ticked in ROADMAP (yaw 30 > the 2-deg bar; 10-min PLAY test deferred to the per-eye
session). All constants in patterns.h (kSceneBuildRva/kSceneBuildPrologue), full map +
derivations in ENGINE_NOTES "Scene-draw architecture".

**Probe tooling extended** (`reentry hook [build|submit|drain|flush]`, `reentry dump
<n>` per-call submit arg telemetry, `reentry arg3` call-site filter; submit doubles with
copied loc/rot args, build doubles with original args + CalcView second-pass yaw; the
build slot owns the double-call controls while enabled). Capstone-based scratchpad
disasm workflow replaced hand byte-walking (findings summarized in ENGINE_NOTES, dumps
never committed).

**Session 6 part 2 - SequentialReentry STEREO built end to end; blocked on ONE
reproducible engine deadlock.** `reentry stereo on` (M4 rung 2) is fully wired and
flat-verified mechanically: every build doubled L-then-R (pass 1 caches the driven
camera + applies -IPD/2; pass 2 replays the cached base + IPD/2 - one head sample per
pair), each nested submit pushes its eye tag through the new `vr::sr_push_eye` SPSC
ring, and Present-tail pops one tag per present, capturing into the existing AER eye
swapchains (mono/AER paths untouched; presents without tags flow as before). Flat
proof: eye-offset frame renders (img-diff 2.0 mean vs 0.33 floor) and consecutive
captures are phase-consistent (0.35 - same eye every time). Design rationale in the
ARCHITECTURE decision log.

**The blocker, run to ground across the whole evening**: continuous doubling
deadlocks the THREADED renderer's event protocol (five runs, 16 s - 3.5 min, always
the same thread-dump signature: game thread in an INFINITE "render done" wait at
exe+0x61D38E vs render thread waiting inside the drain). Everything tried and its
verdict, all live: start-state gating (frame-id gate, ring-counter gate) - runs full
rate, does not prevent the hang; watchdog event re-kicks - detection is reliable but
kicking a desynced protocol CRASHES the drain (now detect-only by default, `reentry
wdkick on` to re-arm); poking the flush-point's first mode check `[0x1375BD4]` - it
is a 500-reference GIsEditor-class global, not a render toggle, crashed the next
load (dead end, recorded). **The breakthrough: `-onethread`** (Steam launch arg,
rides the steam://run URL) boots the engine's NATIVE single-threaded renderer - no
pump, no queue thread, no events, deadlock class structurally gone, and FASTER than
threaded in the test scene (630-710 fps vs ~530). Stereo on that substrate ran clean
at 194 pairs/s for ~23k pairs, then hit the ONE remaining defect: a rare crash at
drain+0x33 (fault addr 0x40 - a null object's +0x40 field, the recurring session-5
signature). Minidumps preserved in `%LOCALAPPDATA%\BioshockVR\crash\`
(bvr_20260724_181619.dmp is the onethread-stereo specimen). Stereo stays
command-gated experimental; NOT headset-safe until the null-deref is fixed.

**Nothing reached headset-testable state this session** (the stereo pipeline is
mechanically proven flat but deadlock-blocked; AER remains the working in-headset
stereo).

## Previous state (2026-07-24, session 5)

**DR-5's question is answered; the double-render seam is FOUND but not yet double-called.**
The session-4 command-queue model was corrected by live hooks (all command-gated - default
runs stay unhooked): the renderer is TWO-threaded. The game thread builds the frame and
SUBMITS it (camera loc/rot stored to globals, SetEvent kick); a dedicated render thread
sits in a pump loop (0x61D1D0, entered once - which is why hooking session 4's "frame
root" 0x61D0F0 caught zero calls: that function is a flush/join). The DRAIN (0x61CAE0) is
the real per-frame render entry - drain/s == presents/s exactly (517-525 live, 40 s soak,
~1.6 ms/frame), sole caller the pump. CalcView runs entirely on the game thread (0 calls
inside the drain) - so render-side re-entry can never re-sample the camera, and a drain
double-call pulse faulted at drain+0x33 (SEH-caught, poison latch worked as designed) then
wedged the pump's event protocol (hang, killed). **The SequentialReentry seam is the
game-thread frame SUBMIT at RVA 0x585AC0**: ret 0xC, camera loc by pointer in arg1, rot
copied to the submitted-frame block, TryEnter/Leave CS buffer sync, SetEvent at +0x1A2
(found via the probe's process-wide SetEvent caller sampler; fires ~3.7x per present at
the menu - per-arg telemetry needed before double-calling). Full corrected map + all RVAs
in ENGINE_NOTES "Scene-draw architecture"; constants in patterns.h (kFrameSubmitRva).

**Probe tooling shipped** (`game/bioshock1r/scenedraw.{h,cpp}`, seam commands `reentry
...`): command-gated MinHook slots (drain/flush), SEH-guarded double-call with poison
latch, 1 Hz heartbeat (entries/s, presents/s, tids, durations, caller RVAs), SetEvent
caller sampler (`reentry kick`), one-shot game-thread stack scan (`reentry calcstack`).
Supporting core additions: `d3d11_hook::present_count()`, `frame_inspector::
draw_call_census()`. Two clean soaks passed; the only crash was the intentional
drain-pulse probe (caught + logged exactly as designed).

**Nothing reached headset-testable state this session** (flat probe work only).

## Previous state (2026-07-24, session 4)

**The FOV problem is fully closed, flat-verified end to end.** The live settings object
(`UShockUserSettings`) is located at runtime by scanning the heap for its fixed-RVA vtable
(no stable static pointer exists - ENGINE_NOTES), its int32 `HorizontalFOV` at +0x8C is
what the renderer consumes EVERY FRAME, and:
- **Auto-claim**: the CalcView detour reads it per frame and feeds the projection claim -
  the manual claimed-FOV slider is no longer required (kept as an override).
- **Write past the cap**: `gfov <deg>` (command/overlay slider) writes it per frame with
  save/restore; **flat-verified rendering at 137** (the options UI's 130 is UI-only, no
  code clamp; monotonic img-diff 117 -> 130 -> 145). "Force headset FOV" now writes this
  real control when VR-driving.
- **USER-VERIFIED IN-HEADSET (2026-07-24, same day)**: auto-claim solid with the manual
  slider untouched, and game-FOV write at 137 "very good" (lands on the ~137 headset
  target). Both checklist items passed on the first try.

**DR-3 done in-tree** (RenderDoc never needed): new `core/gfx/frame_inspector` hooks the
context vtable's draw/clear slots; `dumpframe full` writes a one-shot frame dump (RT descs,
VS b0 readback, callstack RVAs, auto-summary + lifetime call census). Findings in
ENGINE_NOTES "D3D11 frame map": HDR R11G11B10 main pass + D24S8, half-res effects pass,
shadow pair, view-proj matrix in VS b0 bytes 128-191 (fov-scaling cross-check EXACT), and -
the headline - **the renderer is a command queue** (executor 0x61C8E0 / drain 0x61CAE0 /
frame root 0x61D0F0, all byte-verified). SequentialReentry must therefore re-enter the
command BUILD, not the drain - that redefines the DR-5 probe (next session, frame root
first).

**New tooling this session** (TESTING.md has workflows): value scanner + poke/ptr/hexdump/
strscan commands behind the command seam (found the settings object via option-change
narrowing + poke A/B), `game-cmd.ps1` (focus-safe command writes), `img-diff.ps1`
(automated A/B verdicts). Debug-CRT gotcha recorded: sprintf_s asserts modally on overflow -
use _snprintf_s/_TRUNCATE for untrusted bytes (froze the game once).

**Known flake (unresolved, low-rate)**: one boot crashed 0xC0000005 at
`bioshockvr.dll+0x30BE5` during init (before the SEH guards landed on the vtable sweep;
has not recurred since). If it recurs, the crash filter now logs module+RVA + fault addr -
symbolize against the build PDB.

**User checklist - COMPLETED 2026-07-24 (both items passed, see Current state):**
1. ~~Auto-claim check~~ - solid with the manual slider untouched.
2. ~~FOV 137 in-headset~~ - "very good".
3. Still optional any session: Steam Link cross-check; 4:3 resolution experiment
   (see session-3 notes).

## Previous state (2026-07-23/24, session 3)

**M4 rung 1 (AlternateEye) code is in on top of the verified M3 drive.** `core/vr` now owns a
PAIR of backbuffer-sized swapchains (index 0 still serves quad + mono projection). With camera
mode on, the "AlternateEye stereo test (judders)" checkbox alternates which eye each game frame
renders: CalcView shifts the camera by `sign * IPD/2 * worldScale` along view-right
(`vr::current_eye_sign()`), Present-tail copies the backbuffer into that eye's swapchain and
stores that eye's located pose, and submission gives each eye its LATEST held image + stored
pose so the compositor reprojects the half-rate-stale eye (judder, not flicker; mono fallback
until both eyes hold an offset image). The sign is published AFTER submit and only flips when an
offset frame was captured, so the un-offset enable frame is never mislabeled (see ARCHITECTURE
decision log). New controls: IPD slider (55-75 mm, default 63), "Swap eyes" inverted-depth
diagnostic, `(AER eye L/R)` tag on the layer line, and a "head offset (UU)" telemetry readout
that turns the world-scale question into a number. Flat path re-verified live this session
(scan RVA 0x1BE7A0, hook + heartbeat, VDXR instance, quiet no-headset retry, clean exit).

**M4 rung 1 USER-VERIFIED (2026-07-24): "parallax and other stuff are very nice."** The
winning configuration: the remaster's **FOV video option at 130** (its max) + the overlay's
**manual claimed-FOV slider at 130** (matching), VR camera mode + AlternateEye on. The user's
verdict: "everything is very good" - solid geometry, real parallax, depth not inverted. M3 is
now fully ticked too (6DOF verified session 2, geometry verified today). The whole fov saga is
resolved and recorded in ENGINE_NOTES: the PC+0xE0 field is telemetry-only (renderer never
reads it - proven by automated A/B screenshot sweeps via the new command-file seam +
tools/game-shot.ps1), the video option is the only real control, and the claim must match it
by hand until the settings object is scanned. Ini FOV locks back at stock True.

**Open item from the verification**: the user suspects the **IPD slider may not be doing
anything perceptible** - deferred by their choice. Note for that investigation: at world scale
50 UU/m, the 55-75 mm range only moves each eye by ~0.5 UU total, and perceived depth scale is
driven by the worldScale-to-IPD ratio - so a worldScale miscalibration would mask the slider.
Verify with an exaggerated test value (e.g. temporarily widen the slider range) before
concluding it is broken.

**User checklist (optional, any session):**
1. Steam Link cross-check - set SteamVR as active OpenXR runtime and repeat the M2 checklist.
2. If the thin top/bottom black bands bother: try the highest 4:3 resolution at FOV 130
   (vertical coverage becomes complete at a sharpness cost).

## Previous state (2026-07-23, session 2)

**DR-4 fully retired and M2 user-verified on the Virtual Desktop path.** In-headset: big
head-tracked screen, gamma OK (sRGB pick correct), sliders work, clean flat fallback. In-game:
wobble, offsets, yaw, FOV override all visible; no stutter, crashes, or input weirdness. M3
landed the same session and its 6DOF drive was user-verified (rotation, lean, turn, recenter);
immersion/world-scale judgment deferred to M4 stereo. `core/vr/openxr_runtime.cpp` runs the
whole xr_hello32 flow in-process: instance at init, lazy session on the game device with a 5 s
retry (**connect Virtual Desktop mid-game and it comes up without restarting**), xrWaitFrame
pacing at Present head, backbuffer copy at Present tail. The game pauses its boot sequence
while unfocused (see ENGINE_NOTES) - foreground the window in automated tests. The FName-chain
scan (`core/hooks/pattern_scan.cpp`, itsloopyo MIT attribution) resolves `eventPlayerCalcView`
at **RVA 0x1BE7A0** (exe rebased under ASLR; RVA is the stable identifier). The detour fires
every frame incl. main menu, call rate can exceed fps (heartbeat 400-7800 calls/s, default ON
during bring-up). Adapter UI flows through the `IGameAdapter` seam.

## Previous state (2026-07-23, session 1)

M0 complete and user-verified (F10 overlay, D3D11 FL 11_0 confirmed, LAA yes). DR-1 fully
retired: xr_hello32 (32-bit) ran a complete OpenXR session on VDXR 1.0.10 with the Quest 3
(60 frames, RTX 4060 LUID match) - M2 is unblocked. DR-2 done. Repo public at
https://github.com/mohamad-balouza/bioshock-vr. Em dashes banned repo-wide.

## Next steps

1. **USER: in-headset test of the one-toggle flow** - checklist at the bottom of the
   session-8 log. Load a save, `vrstereo on` (or the overlay checkbox) once, headset
   on; loads no longer need any off/on dance. Confirm stereo/comfort match session 7
   and that a mid-play save load stays stereo. This is the M4 30-min done-when run.
2. **Combat-scene perf check**: session-8 flat perf was measured in spawn/arrival
   scenes (~81-225 pairs/s, both > 72). Get a combat save and confirm pairs/s stays
   over 72 under effects (the only untested M4 perf case).
3. **Remaining stereo polish**: HUD-in-stereo decision (renders in both eyes; verify
   on a HUD-bearing spawn, M9 ties in), world-scale/IPD calibration pass (parked M9).
4. If the init-crash flake (bioshockvr.dll+0x30BE5, one occurrence, pre-SEH-guards)
   recurs: the crash log now prints module+RVA - symbolize against the PDB and fix.
5. Still open from M3: cutscene cameras are head-driven too (may need a viewactor == pc
   guard).
6. DR-7: borderless/windowed stability; DR-6: menu input path (session-5 note: synthetic
   clicks sometimes only highlight a gameswf item - VK_RETURN activates it, TESTING.md).
7. Optional anytime: Steam Link / SteamVR cross-check.
8. **Parked in M9 (user's call, 2026-07-24): IPD slider verification** - exaggerated-offset
   test first, world scale before IPD (perceived depth scale is the worldScale/IPD ratio).

## Open questions / blockers

- ~~PC+0xE0 FOV recompute~~ - moot: the field is telemetry-only (2026-07-24, ENGINE_NOTES);
  the engine re-stamps it to 100 on scene/controller changes, which is harmless to us.
- CalcView call rate >> fps at the uncapped menu - determine whether extra calls are benign
  re-entries (same frame) or distinct view queries; affects where per-frame XR pose sampling
  should live (matters more for SequentialReentry).
- Console availability in the current Steam build unverified - test Tab with `-allowconsole`.
  (Less urgent now: the synthetic-click path works for menus.)
- Adapter VRAM logs as "3072 MB" - DXGI_ADAPTER_DESC.DedicatedVideoMemory is a 32-bit SIZE_T in
  our process, so values ≥4 GB truncate. Cosmetic; ignore.
- itsloopyo's headtracking mod also installs as `xinput1_3.dll` - mutually exclusive with ours
  (install.ps1 backs theirs up automatically).

## Session log (newest first)

### 2026-07-24 - Session 8

- **Step 0 - flush-point disk disasm (capstone, scratchpad).** Re-walked
  0x61D260..0x61D400 from the exe: prologue `55 8B EC 51 8B 4D 0C`, `ret 8`,
  and the INLINE branch confirmed exactly (arg1 -> [mgr+0xC]; arg2's 16 dwords
  -> [mgr+0x10..0x4C]; decision chain -> eax; [mgr+0x50]=eax, [mgr+0x54]=1;
  eax==0 -> `mov ecx,esi; call 0x61CAE0` then straight to the epilogue - no
  post-drain work). Matched the session-7 decode byte for byte.
- **Structural 1t SHIPPED (commit f27a0d0).** `FlushPointDetour` reproduces
  that inline block itself when `g_forceInline` is armed and mgr is non-null,
  calling the drain THROUGH its hooked target (guard + telemetry stay live),
  SEH-guarded (poison + auto-disarm on fault), falls through to the original
  when mgr is null (pre-world). `reentry 1t` repointed at it (no poke); legacy
  poke moved to `reentry 1tpoke`. render_is_threaded() honors the override;
  heartbeat gains `forced/s`, status/overlay gain `1t=hook|poke|off`. Constants
  in patterns.h (kFlushPointRva/prologue/kMgr* offsets), full derivation in
  ENGINE_NOTES "Structural 1t".
- **Flat verification - baseline + soak PASSED.** In gameplay: `1t on` (hook)
  -> mode=1T, drain caller RVA inside bioshockvr.dll (expected), presents
  continue, numerator still 12. `stereo on` -> 239 pairs/s = 478 presents/s all
  on the game thread, guardskips 0, eye-offset img-diff 1.96 vs 0.40 floor,
  phase-consistent 0.40; ~3-min stationary soak (200 heartbeats, 0 anomalies).
- **LOAD-CROSSING - THE HAZARD IS CLOSED.** With 1t + stereo armed end to end:
  (1) in-game save load via LOAD; (2) quit-to-main-menu teardown; (3) new-game
  load (Bink intro -> in-water intro); (4) the bathysphere DESCENT into Rapture
  (real multi-map streaming transition, loc crossed +75000 UU with 1t forced
  the whole way). Zero crashes, zero new dumps, guardskips 0, stereo re-engaged
  on arrival. The session-7 poke crashed a loader on step 1; the hook survives
  all four because the numerator global is untouched.
- **One-toggle `vrstereo on|off` SHIPPED (commit 1a821a0)** - top-level command,
  `reentry vrstereo`, and overlay "VR stereo" checkbox; sequences 1t -> camera
  mode -> stereo, reverses on off, sticky across loads. Overlay checkbox posts a
  request the game thread applies from note_calcview (outside hooked calls - MH
  installs must not run mid-build). New core setter `vr::set_camera_mode`. The
  1t menu refusal was DROPPED (load-proven; pre-world arming is inert).
  Flat-verified: `vrstereo on` at the MENU armed all three (`VRSTEREO READY`),
  a CONTINUE-load carried into Rapture with stereo doubling live and no re-arm,
  `vrstereo off` restored mode=MT / build==presents / drain on the render thread.
- **Perf profile:** Rapture arrival scene ~81 pairs/s = 162 presents/s sustained
  (79-83 typ, one dip to 62), eye-offset 6.5; lighthouse spawn 225 pairs/s. Both
  clear the 72-pairs/s (144 presents/s) M4 target. Combat scene still untested
  (needs a combat save).
- Harness notes: PowerShell `mouse_event` dx/dy must be signed `int` (negative
  turns threw UInt32 cast errors); reused game-hover.ps1 (real WM_MOUSEMOVE for
  gameswf highlight) + game-move.ps1 (relative turn + held key) in scratchpad.
  The user drove the game to the lighthouse/Rapture at points to save time.
- Session ends: game closed, command.txt cleared, DLLs current, all commits
  pushed. New streamlined in-headset checklist below.

**In-headset stereo checklist (session 8 - the ONE-TOGGLE flow; loads no
longer need any off/on dance):**
1. Quest 3 on, Virtual Desktop connected (VDXR runtime), Streamer running.
2. Launch BioShock Remastered flat from Steam (no launch args). If the
   "failed to properly shutdown... revert Options?" dialog appears, click No.
3. `.\tools\game-cmd.ps1 "vrstereo on"` (or tick "VR stereo" in the F10
   overlay's Reentry section). The log must say `VRSTEREO READY (1t=1
   stereo=1)`. You can do this at the MENU or in gameplay - either is fine now.
4. Load your save via CONTINUE (or LOAD). Stereo stays armed across the load and
   re-engages automatically in-game - no commands needed. (At the static menu
   there is no doubling; it starts once gameplay builds run.)
5. Headset on. Verify: real per-eye parallax at full rate, depth correct, world
   solid on head turns, comfort as in session 7 (pair pacing is on; toggle "SR
   pair pacing" in the overlay for a live A/B).
6. Loads are now safe with it armed - load another save or cross a level
   transition freely; stereo persists. EXPECTED (not failures): HUD in both eyes
   on a HUD-bearing spawn, IPD/world-scale not yet calibrated.
7. Bail-out any time: `.\tools\game-cmd.ps1 "vrstereo off"` (full recovery to
   flat threaded) or just kill the game - saves are safe. NOTE: command.txt
   re-applies at boot; the session cleared it, but if you send `vrstereo on`
   and then quit, clear it (or send `vrstereo off`) before the next launch so
   it does not re-arm at the menu.

### 2026-07-24 - Session 7

- **Minidump forensics closed the null-deref in one pass** (scratchpad mdparse.py -
  hand-parsed MiniDumpNormal: streams, x86 contexts, module list, EBP walk +
  ret-scan with module attribution; pdbsym.py - dbghelp symbolization of our DLL
  frames; disasm.py - capstone disk walks). All three 2026-07-24 evening dumps
  decoded: two drain+0x33 specimens fault on the render PUMP thread entering the
  drain with `[this+0xC]` (submitted-frame slot) NULL; the third is the recorded
  0x1375BD4-poke load crash at 0x741D7F. The "onethread" crash specimen had a live
  pump thread + game thread parked in the deadlock wait inside a hooked build +
  watchdog kick frames - a threaded-mode deadlock-then-kick crash, not an
  onethread defect.
- **Session-6's onethread substrate falsified live**: the arg rode the command
  line into the process (WMI-verified) while the drain ran on a hot pump thread
  (96 s CPU), and "onethread" is not in the exe's strings - never parsed. The
  session-6 hexdump verification was a menu-time artifact: the pump globals are
  zero before the first world load in EVERY mode (watched them flip 1T -> MT at
  the save load, same boot).
- **Fix #1 (commit bc1f575)**: drain empty-slot guard (skip `[this+0xC]==0`
  drains - the crash state), stereo substrate gate (`stereo on` refuses threaded;
  `force` overrides), `mode=MT|1T` heartbeat tag, forensic constants in
  patterns.h. The gate proved itself the same session: it refused the first
  post-load stereo attempt (mode had silently flipped to MT).
- **The real single-threaded switch (commit 503a695)**: full flush-point decision
  chain decode -> the hw-thread quotient pair (12/1, 10 refs total, 7 identical
  inlined tests) is the only path to the threaded hand-off. `reentry 1t on` =
  guard first, then poke numerator -> 1. Verified same boot: mode=1T,
  beatTid==calcTid, drain caller 0x61D367 (inline site), submits stop in mono,
  pump never kicked, guardskips 0. Presents 413/s mono (~20% under threaded).
- **Stereo flat verification on the inline substrate - ALL PASSED**: 225 pairs/s
  = 450 presents/s (range 141-532 across scenes), presents exactly 2x builds,
  eye-offset diff 2.03 vs 0.33 floor, phase-consistent captures (0.43), 5-min
  stationary soak (15/15 clean) + ~6.5 min synthetic PLAY in two passes (10 + 3
  clean cycles; WASD/mouse navigation, camera climbed the lighthouse stairs,
  21-35 mean img-diffs between cycles; pass 2 ended by the session wrap, not a
  defect - user waved off further passes) - zero faults, zero new dumps, zero
  watchdog detections. `stereo off` recovery instant (2nd=0, builds 1:1 with
  presents, still inline). Previous best on the threaded substrate was
  16 s-3.5 min to a hang or crash.
- Harness lesson recorded: a stale `command.txt` re-applies at boot (the poller
  saw session-6's "reentry stereo on" and armed everything at the menu) - clear
  it or overwrite before launching for controlled runs.
- **FIRST IN-HEADSET FULL-RATE STEREO TEST - PASSED** (user drove it): real
  per-eye parallax at full rate, depth correct, world scale good - "pretty good
  and working as intended, we can continue". Two follow-ups: (a) eyes feel weird
  on head movement; (b) HUD not seen (that spawn has no HUD).
- **xr-frame-per-pair pacing built for (a)** (commit e90765c): per-present
  xrWaitFrame located each eye of a pair at a different predicted time while both
  images came from one head sample -> motion-dependent reprojection shear + halved
  tick. Now a LEFT present holds the XR frame open and the RIGHT completes it: one
  waitFrame/locate/prediction per pair. Default on, overlay toggle for live A/B.
  Flat regression clean; comfort is the next headset check.
- **Load-path crash found + guarded (same commit)**: arming 1t (or doubling)
  across a save load crashed a loader thread (ntdll EnterCriticalSection null CS,
  load-path stack, no mod frames) - the hw-thread global + the doubled build have
  load-path consumers. Guards: `1t on` refuses at the menu (verified live: the
  refusal fired), warns to off-before-load, and pass 2 doubles ONLY gameplay-caller
  builds. Regression: menu refusal OK, in-gameplay arm clean, 3-min stereo soak
  clean, stereo-off then 1t-off restored threaded mode. The 19:54 dump is that
  crash (recorded, not committed).
- Session ends with the updated (order-corrected) headset checklist below, game
  closed, all commits pushed.

**In-headset stereo checklist (updated session 7 - ORDER MATTERS: load first,
THEN arm 1t):**
1. Quest 3 on, Virtual Desktop connected (VDXR runtime), Streamer running.
2. Launch BioShock Remastered flat from Steam (no launch args - `-onethread`
   does nothing; remove it if set).
3. **Load the save via CONTINUE FIRST** (before arming anything - a load with 1t
   active crashes the loader). Confirm the F10 overlay works.
4. In PowerShell (repo root): `.\tools\game-cmd.ps1 "reentry 1t on"` - overlay/log
   shows `mode=1T` (or `render 1T` in the overlay reentry line). If it says
   "1t refused: no world loaded yet", you are still at the menu - load first.
5. Enable "VR camera mode" in the overlay; confirm the layer line reads
   `projection`.
6. `.\tools\game-cmd.ps1 "reentry stereo on"` - the log must say
   "STEREO ON (single-threaded render)". Put the headset on.
7. Verify: real per-eye parallax at FULL rate, depth correct, world solid on
   head turns - and THIS TIME whether head movement feels right (pair pacing is
   on; toggle "SR pair pacing" in the overlay for a live A/B if it still feels
   off).
8. EXPECTED imperfections (not failures): HUD visible in both eyes (on a
   HUD-bearing spawn), IPD/world-scale not yet calibrated. Game tick should feel
   less halved than before pair pacing.
9. **Before loading another save / changing level**: `.\tools\game-cmd.ps1
   "reentry 1t off"` first (then you may `stereo off` too). Bail-out any time:
   `reentry stereo off` recovers instantly; worst case kill the game - saves are
   safe. NOTE: command.txt re-applies at boot - clear it or send the off commands
   before quitting so stereo/1t do not re-arm at the next menu.

### 2026-07-24 - Session 6

- **DR-5 CLOSED with the full arc in one session**: hook the submit -> prove the
  double-submit is absorbed (presents never double, yawed camera never renders - the
  session-5 seam hypothesis refuted by its own probe) -> follow the submit's live
  caller RVA into the disk image -> find the scene BUILD root 0x4CCE70 -> double-call
  it -> a complete second engine-paced frame per game tick with our camera on it,
  yaw-30 visible in flat captures. Every step flat-harness-verified.
- **Tooling upgrade that cracked it**: installed capstone (pip) + scratchpad PE/disasm
  scripts - the disk-image walk found the submit's true statics (arg2 IS the FRotator*,
  ECX dead, literal ret 0xC, SetEvent site/event object), the decoy 55-8B-EC function
  at 0x4CCD20, the CC-run boundary, and the build's aligned-stack prologue in minutes.
  Hand byte-walking is retired for anything bigger than a spot check.
- **scenedraw extended**: build + submit hook slots (fastcall-passthrough detours for
  `ret 0x10`/`ret 0xC` targets), `dump <n>` per-call arg telemetry with presents-delta,
  `arg3` filter, submit-nested-in-build counter, build-slot priority on the double-call
  controls. Two incremental code commits pushed before the docs wrap.
- **Live numbers (gameplay, save spawn)**: submit 1:1 with presents (single site
  0x4CDD8A; load path uses 0x4CC6C8; static menu: zero - session-5's "3.7x at menu"
  kick figure did not reproduce). Build pass-through ~60 us; doubled second call
  ~2 ms. Continuous doubling: 225 build/s -> 450 presents/s, tick halves, off recovers
  instantly. Noise floor 0.33-0.37; yawed-frame diff 7.7-7.9 mean.
- **Honest stability record**: ~3.5 min continuous doubling clean, then one hang
  (~124k doubled frames, during a game-shot focus cycle; kill + relaunch). Recorded in
  TESTING with hardening candidates queued into the per-eye design.
- **Curious + useful**: flat captures phase-lock to the SECOND present of each doubled
  pair - present order within a pair looks deterministic, which simplifies per-present
  eye attribution for the split.
- Session ends with the game closed (killed post-hang), DLLs current in the game
  folder, no headset items this session.
- **(part 2, same day) SequentialReentry STEREO wired end to end** on user go-ahead:
  `reentry stereo on` = doubled builds with L/R eye offsets (pass-1 cached-base +
  pass-2 replay - one head sample per pair), SPSC eye-tag ring game->render
  (`vr::sr_push_eye`), per-present eye capture into the AER swapchain pair,
  mono/AER paths untouched. Flat-verified: offset frame renders (diff 2.0 vs 0.33
  floor), captures phase-consistent (0.35). ARCHITECTURE decision-log entry written.
- **(part 3, user go-ahead "let's do it") The fix hunt, every step live-tested**:
  watchdog built (depth-gated stall detection + engine event re-kicks) - detection
  perfect, recovery kicks CRASH desynced state (demoted to detect-only, `wdkick`
  opt-in); flush-point head fully disassembled -> its first mode check `[0x1375BD4]`
  turned out to be a 500-ref GIsEditor-class global (poke = load crash, dead end
  recorded); then the win - **`-onethread` launch arg boots the native
  single-threaded renderer**: deadlock class structurally gone, FASTER than
  threaded (630-710 fps), stereo doubles cleanly on top (194 pairs/s)... until a
  rare drain+0x33 null-deref crash (~1 per 23k pairs, the recurring 0x40-fault
  signature). Minidump preserved - next session symbolizes it. Eye tags moved to
  the build detour (mode-agnostic ordering). Session truly wrapped here: game
  closed, 5 code commits + docs pushed.
- **(part 2) The deadlock hunt**: five continuous runs hung (16 s - 3.5 min).
  Built hangdump.py (outside-process Wow64 thread dump) - two specimens show the
  IDENTICAL signature: game thread in WaitForSingleObject(INFINITE) at exe+0x61D38E
  (render-done flag+event wait in the build) vs render thread waiting inside the
  drain. Decoded the frame-id completion-bit pair (0x13AF7E8/+0x10, high bit =
  consumed; a wait-for-minus-one first guess throttled to 48 fps and was corrected),
  the queue ring pointers (+0x118/+0x11C, unequal at idle) vs seg counters
  (+0x128/+0x12C, equal at idle), and the flush-point wait function itself
  (~0x61D340: flag-then-INFINITE-wait, same event class as the pump kick, plus a
  single-threaded inline-drain fallback). Start-state gating falsified twice at
  full doubled rate; the pass-2 gate (frame-ids + counters, bounded, skip-on-
  timeout) is kept as the graceful unfocused degrade. Three concrete deadlock fixes
  queued in Next steps; stereo stays command-gated experimental, not headset-safe.

### 2026-07-24 - Session 5

- **DR-5 probe ran its full arc in one session**: hook the presumed frame root ->
  discover it never fires -> re-aim at the drain -> map the real two-thread architecture
  -> refute render-side re-entry -> locate the game-thread submit seam. Every step
  flat-harness-verified; two clean 40 s+ soaks; the one crash was the intentional
  SEH-guarded drain pulse (poison latch worked; the subsequent event-protocol wedge and
  hang is recorded in TESTING.md as expected probe cost).
- **Corrected architecture (ENGINE_NOTES rewritten)**: game thread builds + submits
  (camera by pointer into globals, SetEvent kick at submit+0x1A2); render thread pump
  loop 0x61D1D0 (thread main, registers its tid in a global) drains once per Present
  (drain/s == presents/s exactly); 0x61D0F0 is a flush/join, not a frame function.
  Session-4's prologue walk overshot the pump's frameless `push esi` entry - lesson:
  the CC-55-8B-EC heuristic misses frameless functions.
- **New instruments that cracked it**: process-wide SetEvent caller sampler
  (`reentry kick on|off`) - one 8 s sample separated the game-thread kick (0x585C68)
  from a 3-worker job pool (0x583FDB); one-shot game-thread stack scan
  (`reentry calcstack`); per-boot import resolution via shared system-DLL bases
  (WaitForSingleObject/SetEvent/GetCurrentThreadId/ReleaseSemaphore/
  RtlTryEnter+LeaveCriticalSection all pinned to IAT slots).
- **Submit function byte-walked** (entry 0x585AC0, ret 0xC, camera loc/rot through
  args/globals, CS-guarded buffer swap) - constants + prologue bytes staged in
  patterns.h for next session's hook. Double-submit with a yaw delta = the remaining
  DR-5 experiment.
- Probe v1 (frame-root) and v2 (drain + kick/calcstack) both committed and pushed
  incrementally; core gained `present_count()` and `draw_call_census()` accessors.
- Harness notes: gameswf CONTINUE needed VK_RETURN (clicks only highlighted - recorded
  in TESTING.md); today's standing-still noise floor measured 0.21-0.40 mean.

### 2026-07-24 - Session 4

- **(end of session) IN-HEADSET VERIFICATION PASSED**: auto-claim solid with the manual
  slider untouched; gfov 137 "very good" in the headset. The session-4 FOV work is fully
  user-verified; session closed with DR-5's hook probe as the next opener.
- **FOV endgame closed.** Built the value-scanner seam (memscan/mempoke/memptr/hexdump/
  strscan + int variants) and img-diff.ps1; narrowed 662 int candidates to 4 by having the
  user change the FOV option through the game UI between rescans; poke + screenshot A/B
  found the consumed copy; RTTI walk named it `UShockUserSettings` (+0x8C int32). Two traps
  documented: the ini value is an INT (float scans blind), and the one .data "static root"
  was a coincidental range hit later overwritten by floats - resolution is now a heap scan
  for the fixed-RVA vtable (cached, revalidated per call, SEH-guarded after one boot crash
  from unguarded reads during heap churn).
- **Auto-claim + gfov landed**: CalcView reads the option per frame into the projection
  claim (manual slider demoted to override); `gfov` writes it per frame with save/restore;
  Force-headset-FOV now writes the real control. Flat A/B: 137 renders wider than 130
  (UI cap is UI-only), restore returns to noise floor. In-headset check = user checklist.
- **DR-3 via in-tree frame inspector** (new core/gfx module; RenderDoc never installed):
  context-vtable hooks on draw/clear/SetRT slots, one-shot lite/full dumps with RT descs,
  VS b0 readback, triple-source callstack RVAs, auto-summary + lifetime census. Frame map
  in ENGINE_NOTES: HDR main pass, half-res effects pass, shadow pair, view-proj in VS b0
  bytes 128-191 (m00 scaled EXACTLY as 1/tan(hfov/2) between 117/137 dumps - independent
  proof the option IS the rendered fov).
- **DR-5 groundwork - architecture finding**: byte-walk of the draw-stack functions shows a
  render COMMAND QUEUE: executor 0x61C8E0 (`void __thiscall`, type id at this+0xC), drain
  loop 0x61CAE0 (site 0x61CD0D), frame root 0x61D0F0 (site 0x61D21E; its call rel32
  byte-verified to the drain). Consequence: SequentialReentry must re-enter the command
  BUILD, not the drain. Hook probe deferred to next session (fresh session; this one had
  two unrelated PC power cuts - user's electricity, not the mod).
- Debug-CRT lesson recorded in TESTING: sprintf_s asserts with a MODAL on overflow (froze
  the game mid-scan); value_scan switched to _snprintf_s/_TRUNCATE. Crash filter upgraded
  to log module+RVA + fault address. tools/game-cmd.ps1 added (focus-safe seam writes -
  the poller only runs while the game window is focused).

### 2026-07-23/24 - Session 3

- **M4 rung 1 (AlternateEye) landed** per the session-2 design handoff: per-eye swapchain pair
  in `core/vr` (index 0 still serves quad/mono), held stale image + stored per-eye pose with
  compositor reprojection, eye sign published after submit with capture-gated flip (the sign
  doubles as image attribution - the un-offset enable frame stays mono instead of being
  mislabeled), `vr::current_eye_sign()` consumed by the CalcView drive for the half-IPD
  view-right shift. New overlay controls: AER checkbox (camera mode only), Swap-eyes
  inverted-depth diagnostic, `(AER eye L/R)` layer tag, IPD slider, head-offset UU telemetry.
  Also fixed in passing: the no-OpenXR stub block was missing `set_rendered_hfov` (latent link
  error).
- **Flat smoke test PASSED live** (game launched/closed via Steam under standing permission):
  log shape identical to session 2 - scan 1 candidate at RVA 0x1BE7A0, hook + heartbeat at
  menu, VDXR instance up, quiet no-headset retry, graceful exit. In-headset AER test = user's
  next step (procedure in TESTING.md, checklist in Current state).
- **First AER in-headset test (still 1024x768)**: AER mechanics verified - `layer: projection`,
  eye L/R tag tracks per frame, depth NOT inverted (1-frame sign attribution holds, swap-eyes
  not needed), no crash. BUT the M3 distortion persists (center-stretch relaxing toward the
  periphery on slow head turns) and blocks parallax/immersion/scale judgment; IPD ruled out as
  the cause. Diagnosis: claimed-vs-rendered fov mismatch that the self-echoing readback cannot
  correct. Landed **"Manual claimed FOV" calibration slider** (claims an arbitrary hfov in the
  projection layer; swim stops when claim == truly rendered fov -> the locked value measures
  the engine's real fov); layer line now shows target/readback/claimed. Calibration procedure
  written into TESTING.md; deriving + baking the engine fov mapping = next step.
- **(07-24) Swim calibration SUCCEEDED and found the cause**: at 1920x1080 the world locked
  solid at claimed ~100 with the field forced to 137, and stayed solid at field=100 unforced ->
  renderer caps hfov at ~100. Live ini: remaster settings pin `HorizontalFOV=100` under
  `HorizontalFOVLock=True` (+ `bHorizontalFOVLock=True`) -> ENGINE_NOTES updated (the PC+0xE0
  readback echoes writes instead of the rendered fov whenever the lock clamps). User also
  reported the expected honest black band at the bottom (uncovered headset fov below center) -
  grows as claimed fov shrinks; closes only when the engine truly renders wider. Landed:
  force-headset-fov **default OFF** (truthful auto-claim = solid world out of the box), manual
  claim slider kept per user request. **Flipped both PC FOV-lock ini flags to False** (backup
  `Bioshock.ini.bvr-bak-fovlock`) - unlock verdict is the user's next flat-screen test.
- **(07-24, later still) M4 RUNG 1 USER-VERIFIED.** With the video FOV option at 130 + manual
  claimed fov at 130: "everything is now perfect... the parallax and other stuff are very
  nice." M3 done-when ticked as well (6DOF verified session 2 + geometry verified today).
  Depth not inverted (swap-eyes never needed - the 1-frame sign attribution holds). Open
  follow-up parked by the user: whether the IPD slider has a perceptible effect (note in
  Current state: at 50 UU/m the 55-75 mm range is a ~0.5 UU change - verify with an
  exaggerated value before concluding). Session wrapped here.
- **(07-24, later) FOV endgame - field retired, real control found.** User reported the FOV
  override slider dead on flat with locks off. Built an automated harness in response: 1 Hz
  `command.txt` seam in the detour (`fov`/`offset`/`recenter`) + `tools/game-shot.ps1`
  (PrintWindow captures) + `tools/game-click.ps1` (synthetic menu clicks; navigated the menu
  and triggered a save load with them). Flat A/B sweeps: field writes 60-140 = pixel-identical
  frames in real gameplay AND the menu attract scene, under BOTH lock states -> PC+0xE0 is
  telemetry-only; DR-4's "override widens view" retracted (VR claim-side artifact - user
  clarified it was only ever observed in-headset). Locks restored True; direct ini
  `HorizontalFOV=137` also did nothing (out of range). **User found the real control: the
  remaster's FOV video option, range 75-130, visibly drives the render on apply.** Field does
  not mirror it -> manual claimed-FOV slider must match the option value for now;
  settings-object scan queued to automate the claim and attempt >130. Payoff run queued:
  option 130 + claim 130 + AER.

### 2026-07-23 - Session 2

- **M3 second in-headset test**: 6DOF drive solid; Force-headset-FOV visibly changed the
  image; user cannot judge immersion/world-scale without per-eye stereo -> M4 AlternateEye
  promoted to next session's goal (full design in Next steps). A started AER refactor was
  reverted cleanly (session wrap; HEAD == installed build). Session totals: 13 commits -
  DR-4 complete + user-verified, M2 complete + user-verified (VD path), M3 code landed with
  diagnostics, M1 down to DR-3/5/6/7.
- **M3 first in-headset test + fix round**: drive worked; distortion/no-depth/scale reports
  led to: projection-readiness gate (mixed quad+drive state now impossible), rendered-fov
  readback claims (fisheye self-correction), resize-swapchain-recreate fix, layer/fov
  diagnostics in overlay + log, Force-headset-FOV escape hatch.
- **M3 code landed (same session, after the M2 pass)**: core/vr locates head pose + per-eye
  views at Present-head (VIEW-space xrLocateSpace + xrLocateViews), computes the circumscribed
  headset FOV, and swaps quad -> projection layer in camera mode. camera.cpp converts XR
  meters/quat -> UU/FRotator (conventions documented in-file, roll sign pre-validated by the
  user's slider test), drives loc/rot/FOV in the detour, adds World scale + Recenter. Pose
  crosses threads under a mutex; pull-based access (see ARCHITECTURE decision log). Flat path
  re-verified live. In-headset M3 test = next session's first item.
- PowerShell 5.1 gotcha hit: double quotes inside git commit -m here-strings mangle argument
  passing - use a message file + git commit -F for multi-line messages with quotes.
- **User verification pass (end of session): M2 VD path + DR-4 both PASSED.** In-headset: big
  screen appeared, gamma OK, sliders work, clean flat fallback. In-game: wobble, offsets, yaw,
  FOV override all visible. No stutter/crash/input issues. M2 lacks only the optional Steam
  Link cross-check; roll-offset visual check noted as a small open item for M3.
- **M2 stretch landed after DR-4**: `core/vr/openxr_runtime.{h,cpp}` ports the xr_hello32 flow
  in-process (instance at init; lazy session on the game device with 5 s retry; event pump;
  quad swapchain sRGB-preferred; Present head = waitFrame/beginFrame, tail = CopyResource +
  endFrame; kill switch + screen sliders in the overlay). Verified live: instance on VDXR
  1.0.10 inside the game, quiet no-headset retry, DR-4 hook and game unaffected. In-headset
  test = user's next step (TESTING.md M2 procedure written).
- Found + recorded: game boot pauses while the window is unfocused (foreground it in tests).
- **DR-4 landed**: studied itsloopyo's `memory.rs`/`engine_hook.rs` (MIT), ported the FName-chain
  scan as generic `core/hooks/pattern_scan.{h,cpp}` (module capture, wide-string/imm32 sweeps
  via VirtualQuery region walk, E8 -> 89 0D global extraction, CC CC CC 55 8B EC prologue walk,
  200-byte init-site filter), consumed from the new `game/bioshock1r/patterns.cpp`.
- Created the `IGameAdapter` seam (minimal, grows per milestone) + `Bioshock1RAdapter`;
  framework wires `game::init_adapter()` between MinHook init and the D3D11 hooks; every
  failure path fail-soft.
- Hook detour (`__fastcall` dummy-EDX): original first, relaxed-atomic telemetry, offset/wobble/
  FOV-override application, one-shot first-fire log + 1 Hz heartbeat (default on). Overlay draws
  the adapter section through the seam.
- **Smoke-tested live twice** (game closed/relaunched via Steam with user's standing permission):
  scan resolved RVA 0x1BE7A0 both runs (rebased base 0x0FB20000 - ASLR active, scan
  relocation-transparent), hook fired at menu, heartbeat 400-7800 calls/s. Recorded in
  ENGINE_NOTES: fires at main menu, call rate >> fps, `AActor**` signature correction, FOV 100.0
  read live.
- 4 code commits + this docs commit pushed incrementally to main.

### 2026-07-23 - Session 1 (continued)

- User confirmed the overlay visually (screenshot: main menu, 500 fps). Toggle key changed
  Insert -> F10 (user's keyboard lacks Insert). Verified live by the user.
- Em-dash ban added (global user preference): they broke PowerShell 5.1 parsing (BOM-less
  UTF-8 read as ANSI) and mojibaked the log and ImGui text. Repo swept to ASCII hyphens.
- **DR-1 retired with a FULL PASS**: wired the OpenXR loader (static, CRT override needed -
  see ENGINE_NOTES), built xr_hello32 (32-bit), found VDXR ships a 32-bit runtime, and with
  the Quest 3 connected ran a complete session: Meta Quest 3 system, FL 11_0, RTX 4060 LUID
  match, 60 frames pumped. VDXR path proven; 64-bit companion fallback not needed.

### 2026-07-23 - Session 1

- Researched game installation (32-bit DX11 Vengeance/UE2.5, no DRM, gameswf Flash UI), engine
  family (BS2R same engine; Infinite UE3-6829), modding ecosystem, prior VR art (vorpX G3D works;
  itsloopyo headtracking hook proven; REFramework MIT reference), and the VR injection stack
  (OpenXR-first for VDXR+SteamVR). Full findings → RESEARCH.md.
- Decided architecture (C++20/x86, xinput proxy → bioshockvr.dll, core+adapter split, stereo
  ladder with SequentialReentry as primary bet) → ARCHITECTURE.md decision log.
- Built: repo + docs suite, CMake (VS2022 `-A Win32`, submodules minhook/imgui/OpenXR-SDK pinned),
  xinput proxy (ordinals verified against the real SysWOW64 DLL with dumpbin - game imports @2/@3),
  mod DLL (deferred init, logger, minidump handler, MinHook, kiero-style Present/ResizeBuffers
  hooks, ImGui overlay), tools scripts.
- **In-game smoke test passed** on first run (full init chain + D3D11 device info in log).
  Found + fixed: logger file locking (fopen_s denies sharing → switched to `_wfsopen` with
  `_SH_DENYNO`), non-ASCII mojibake in log lines, missing `-Install` passthrough in build.ps1.
- Verified: LAA=YES; D3D11 confirmed at runtime; user ini path confirmed after first launch.
- Repo created and pushed: https://github.com/mohamad-balouza/bioshock-vr (public, MIT).
