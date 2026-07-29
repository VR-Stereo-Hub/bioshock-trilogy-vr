# Session start prompt - M10: BioShock 2 Remastered adapter

> Paste this as the opening message of a fresh session. Everything below was verified
> on 2026-07-29 at the end of session 23; re-verify anything that smells stale.

---

## The task

Start **M10: the BioShock 2 Remastered adapter** (`src/game/bioshock2r/`). BioShock 1
support is released and stable at v0.4.1; BS2 is the second game the architecture was
always designed for. The goal this session is **M3-level parity on BS2: a 6DOF head
camera in the headset**, plus whatever core/adapter seam leaks that exposes.

Game: `D:\SteamLibrary\steamapps\common\BioShock 2 Remastered\Build\Final\Bioshock2HD.exe`
(note: **D: drive**, unlike BioShock 1 on K:). Steam appid is in `steam_appid.txt` there.

## Read first (project protocol, from CLAUDE.md)

1. `docs/STATUS.md` - current state and session log (session 23 is the newest entry)
2. `docs/ROADMAP.md` - the M10 section
3. `docs/ARCHITECTURE.md` - core/adapter contract + the decision log at the bottom
4. `docs/ENGINE_NOTES.md` - **the BS1 knowledge base; this is your map.** Every signature
   is documented *with its derivation method*, which is exactly what you re-run on BS2.
5. `git log --oneline -10`

## Hard rules (non-negotiable, from CLAUDE.md)

- **NEVER commit game-derived content**: no decompiled UnrealScript, no extracted assets,
  no captures, no minidumps (they contain game memory). Summarize in `ENGINE_NOTES.md`.
- **32-bit (Win32) only.** Both games are x86.
- Engine addresses/signatures live ONLY in the per-game `patterns.cpp`/`patterns.h`, and
  every one gets documented in `ENGINE_NOTES.md` with how it was derived.
- No code from UEVR (concepts only). REFramework (MIT) may be adapted with attribution.
- Commit style: plain conventional commits, imperative, subject <= 72 chars, **no trailers**
  (no Co-Authored-By, no "Generated with"). No em dashes anywhere - plain hyphens only.
- End of session: rewrite STATUS.md "Current state"/"Next steps", append a dated session-log
  entry, tick ROADMAP boxes, commit, push. A session that ends without pushing STATUS.md is
  a failed handoff.

## De-risking already done - do NOT redo this

Verified 2026-07-29 by comparing the two PE files directly:

| | BioShock 1 | BioShock 2 |
|---|---|---|
| machine | x86 (0x14C) | **x86 (0x14C)** |
| LARGE_ADDRESS_AWARE | yes | **yes** |
| ImageBase | 0x10900000 | **0x10900000** (identical) |
| SizeOfImage | 0x1677000 | 0x1FEA000 (BS2 is bigger) |
| PE TimeDateStamp | 2022-04-13 16:16:54 UTC | **2022-04-13 16:00:37 UTC** |
| imports `xinput1_3.dll` | yes | **yes** |
| imports `d3d11.dll` | yes | **yes** |

**BS2 was built 16 minutes before BS1, from the same engine, in the same build session.**
Two consequences:

1. **The `xinput1_3.dll` proxy injection vector works unchanged.** No new shim needed.
2. Structures and code shapes should transfer closely. Offsets and RVAs will NOT - derive
   them fresh, never copy a number across.

**Every BS1 scan anchor exists in BS2**, with the same shape:

| anchor | BS1 | BS2 |
|---|---|---|
| `PlayerCalcView` (wide) | 1 match | **1 match** |
| `AHands` / `APlayerWeapon` / `UShockUserSettings` / `SkeletonInstance` RTTI | present | **present** |
| `ShockPlayer`, `GamepadPlayerInput` | present | present |

## Candidate vtable RVAs for BS2 - already derived

Derived from MSVC RTTI (TypeDescriptor -> CompleteObjectLocator -> vtable). **The method
was validated by reproducing all four of BioShock 1's known-good values exactly**, so treat
these as high-confidence starting points - but still confirm each at runtime before relying
on it (a live object's `dword0` must equal `imageBase + RVA`).

| class | BS1 (known good) | **BS2 (candidate)** |
|---|---|---|
| `AHands` | 0xD8A28C | **0x1125478** |
| `APlayerWeapon` | 0xD8FF58 | **0x112CC78** |
| `UShockUserSettings` | 0xDA3878 | **0x11523D8** |
| `SkeletonInstance` | 0xE19ACC | **0x10D0FC0** |
| `AShockPlayer` | 0xD82BB8 | **0x11197C0** |
| `AShockPlayerController` | 0xD81C84 | **0x1117BF0** |
| `UGameEngine` | 0xE0DFDC, 0xE0DFF4 | **0x10BD7DC, 0x10BD9E8** |

(BS1's `console_exec` uses the *second* `UGameEngine` vtable, 0xE0DFF4 - expect the same
pattern on BS2, i.e. 0x10BD9E8, but verify.)

The derivation script is disposable and lives in the session scratchpad; if you need it
again, it is ~40 lines: find `.?AVClassName@@`, TypeDescriptor = name - 8, find the dword
equal to its VA (that is COL+12), then find the dword equal to the COL's VA (that is
vtable-4).

## Known architectural work this milestone forces

**`init_adapter()` currently lives inside `bioshock1r_adapter.cpp`
(`src/game/bioshock1r/bioshock1r_adapter.cpp:58`) and hardcodes the BS1 adapter.** Its own
comment says the multi-game dispatch moves out "when the second adapter lands" - that is
now. Expect to:

- add a dispatcher (probably `src/game/adapter_registry.cpp`) selecting by host exe name
  (`BioshockHD.exe` vs `Bioshock2HD.exe`), keeping the fail-soft contract: any scan failure
  logs, reports zero capabilities, and the game still runs flat;
- add `src/game/bioshock2r/` to `src/CMakeLists.txt`;
- give `tools/install.ps1` / `uninstall.ps1` / `build.ps1` a way to target the BS2 folder
  (they default to the BS1 K: path today);
- record every core/adapter seam leak you find in the ARCHITECTURE decision log - that is
  an explicit M10 acceptance criterion.

## Suggested order of work

1. **Adapter dispatch + skeleton `bioshock2r`** that scans nothing and reports zero caps.
   Prove the DLL loads into BS2 and logs its `build:`/`env:` lines. Cheapest possible
   "we are injected" milestone.
2. **CalcView hook.** Re-derive `eventPlayerCalcView` on BS2 the way ENGINE_NOTES documents
   for BS1 (wide-string `PlayerCalcView` -> string xref -> the FName-index global ->
   the event function). Log camera loc/rot/fov at 1 Hz and confirm it tracks the game.
3. **6DOF camera write** (M3 parity): head pose into the camera, recenter, world scale.
   This is the milestone's "done" bar for the session.
4. Only then look at FOV, stereo (M4), aim, hands.

## Traps from BS1 that will bite again

- **CalcView fires at the main menu**, with the view actor being the PlayerController
  itself rather than a pawn. BS1 needed an explicit strict-gameplay predicate
  (`AShockPlayer` vtable identity) plus a deliberate menu-attract escape hatch. Expect the
  same shape and decide it consciously rather than inheriting it by accident.
- **Menu CalcView call rate is uncapped** (~7000/s observed on BS1). Anything you hang off
  the detour runs that often. Throttle from the start.
- **Full-VA heap scans are expensive** - a scan on a grown heap takes seconds and visibly
  freezes the game. BS1 needed backoff *and* dormancy. Do not ship a scan on a cadence.
- **The game is LAA**: actors allocate above 2 GB after streaming, so scans must walk the
  full 4 GB range, not stop at 0x7FFF0000.
- **Steam's `CSERHelper.dll` displaces our unhandled-exception filter** ~0.16 s after the
  first Present. `crash::rearm()` already handles this - do not be surprised by the
  `filter had been displaced` log line, it means the fix is working.
- Engine `Exec` through a hand-built `FOutputDevice` stub is a stack-imbalance hazard
  (documented in `console_exec.cpp` and `patterns.h`); re-derive `kEngineExecRva` for BS2
  and prologue-gate it rather than trusting a fixed RVA.

## Testing

- `.\tools\build.ps1` (Debug) / `-Release`; CMake is not on PATH, the script finds the
  VS-bundled one. `.\tools\tail-log.ps1` follows the log.
- Log + config live in `%LOCALAPPDATA%\BioshockVR\` **shared between both games** - decide
  early whether BS2 needs its own subfolder, or the two games will fight over
  `vrpreset.ini` / `hands.ini` / `weapons.ini`. **This is a real design decision, not a
  detail** - BS1's calibration must not be clobbered by a BS2 session.
- `bioshockvr.prev.log` keeps the previous run; crash dumps land in `crash\`.
- Full procedures incl. Quest 3 / Virtual Desktop setup: `docs/TESTING.md`.
- The user tests in-headset from numbered checklists and reports back. Flat checks must run
  under `vrstereo` and measure stereo quantities from the log - never claim a camera works
  from a mono screenshot.

## Definition of done for M10 (from ROADMAP)

- [ ] `src/game/bioshock2r/` adapter with its own `patterns.cpp`; near-total core reuse
- [ ] M3-level (6DOF mono) within one session of scan work; M4-level stereo within the
      milestone
- [ ] Every core/adapter seam leak found -> ARCHITECTURE decision log
