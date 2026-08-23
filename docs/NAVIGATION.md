# Doc navigation: what to read, and how much of it

The knowledge base is ~2 MB of Markdown across 15 files. Read the wrong way it
costs a whole context window before any work starts; read the right way a
session start is under 5,000 tokens. This file is the router.

**It is information, not instruction.** Nothing here tells anyone how to work -
it says what is in which file and how each one is organised, so the answer can
be found without opening the file that contains it.

---

## The one rule

**Grep an anchor, read a window. Never open a doc whole above ~400 lines.**

Every file below except `TROUBLESHOOTING.md` and `FREEZE_HANDOFF.md` is over
that line. Sizes, measured 2026-08-22:

| File | Lines | Bytes |
|---|---:|---:|
| `STATUS.md` | 11,842 | 826 K |
| `bioshockinfinite/ENGINE_NOTES.md` | 4,419 | 303 K |
| `bioshock1/ENGINE_NOTES.md` | 3,615 | 240 K |
| `bioshock2/ENGINE_NOTES.md` | 2,478 | 167 K |
| `ARCHITECTURE.md` | 1,485 | 120 K |
| `bioshockinfinite/TESTING.md` | 1,408 | 92 K |
| `ROADMAP.md` | 959 | 77 K |
| `bioshockinfinite/ROADMAP.md` | 839 | 66 K |
| `VERIFICATION.md` | 575 | 39 K |
| `bioshock2/TESTING.md` | 524 | 33 K |
| `bioshock1/TESTING.md` | 480 | 33 K |
| `RESEARCH.md` | 357 | 27 K |
| `RELEASE_NOTES.md` | 310 | 19 K |
| `TROUBLESHOOTING.md` | 244 | 13 K |
| `bioshock2/FREEZE_HANDOFF.md` | 95 | 6 K |
| `PORT-CANDIDATES.md` | 79 | 8 K |

---

## Starting a session

```
sed -n '1,150p' docs/STATUS.md          # the live handoff, ~2.5k tokens
git log --oneline -10
```

That is the whole start. **Do not read `STATUS.md` past line 150** unless you
are looking for a specific past session - lines 2268 to the end are 45 stale
"Previous state" blocks going back to session 1, and nothing routes to them.
`grep -n "session 3[0-9]" docs/STATUS.md` finds one if you need it.

Then read the ONE milestone you are working on, not the whole ladder:

```
grep -n "^## M7" docs/ROADMAP.md        # then sed that range
```

---

## Routing by intent

| What you need | Where | How to get it |
|---|---|---|
| Current state, next step | `STATUS.md` | `sed -n '1,150p'` |
| Milestone acceptance criteria | `ROADMAP.md` (BS1/BS2), `bioshockinfinite/ROADMAP.md` (I0-I11) | grep `^## M<n>` / `^## I<n>`, read that section |
| An offset, address, signature, class layout | `<game>/ENGINE_NOTES.md` | grep the field or function name |
| Why a module is shaped the way it is | `ARCHITECTURE.md` | grep `^## `, read one section |
| A past design decision and what settled it | `ARCHITECTURE.md` § *Decision log* | it is at the **bottom** - `sed -n '231,$p'` |
| How to verify something without a headset | `VERIFICATION.md` § 1 *decision table* | `sed -n '22,57p'` first - it routes onward |
| What still needs a human in a headset | `VERIFICATION.md` § 6 | `sed -n '567,$p'` |
| Install / launch / test a game | `<game>/TESTING.md` | grep the milestone or symptom |
| Whether a BS1 behaviour is safe to turn on for BS2/Infinite | `PORT-CANDIDATES.md` | read whole - it is short, and each row carries the opt-in line |
| A user-reported bug | `TROUBLESHOOTING.md` | small enough to read whole |
| Prior art, legal, runtime research | `RESEARCH.md` | grep the topic |
| What shipped in a version | `RELEASE_NOTES.md` | grep the version |

---

## Per-file: how each one is organised

### `STATUS.md` - handoff
Newest first. Lines 1-150 are live. Everything below ~line 2268 is historical
state blocks, newest to oldest. **Prepend when writing; never rewrite in place.**

### `<game>/ENGINE_NOTES.md` - the knowledge base
**Two halves, and this is the thing to know.** The first ~1,450 lines are
organised by TOPIC and are browsable:

```
Process / module layout · Signature / symbol table · Known structures &
conventions · Hook points · Config / ini facts · OpenXR runtime facts ·
D3D11 frame map · Gamepad architecture · Native function table · Fire flow /
aim · Viewmodel / AHands · Skeleton / bone internals · Foreground scene FOV ·
Body facing / control rotation · Desktop present / mirror · Reticle + SET seam ·
UnrealScript findings
```

After that it switches to CHRONOLOGY - `## Session 19`, `## Session 20`, and so
on - so a topic added later is **not** findable by browsing headings. Grep the
identifier instead: `grep -n "kSkelInstBones" docs/bioshock1/ENGINE_NOTES.md`.

Every entry carries its derivation. **Never copy a number between games** -
same engine tree, different link.

### `ARCHITECTURE.md`
Seven sections: Overview · The core/adapter contract · Per-frame orchestration ·
Stereo strategy · Input · VR runtime · **Decision log** (line 231 to the end,
dated entries, newest last).

### `ROADMAP.md`
One `## M<n>` section per milestone with checkboxes and acceptance criteria.
Infinite has its own ladder in `bioshockinfinite/ROADMAP.md` (I0-I11), separate
from M0-M10.

### `VERIFICATION.md`
Section 1 is a decision table mapping intent → tool → command. Read that first;
it tells you which of the remaining sections you need. Sections 2-5 are the
simulated runtime, the agent workflow, numeric thresholds and failure modes.
Section 6 is what only a headset can answer.

### `<game>/TESTING.md`
Install, launch, per-milestone checklists, crash triage. BS1 is the full
version; BS2 and Infinite are deltas against it.

---

## Where things get written

| New knowledge | Goes to |
|---|---|
| A measured offset, address or layout | `<game>/ENGINE_NOTES.md`, with its derivation |
| A design decision someone might reverse | `ARCHITECTURE.md` § *Decision log*, dated |
| A falsified approach | Next to the claim it kills, with the measurement that killed it |
| Session outcome | `STATUS.md`, prepended |
| A milestone closing | Tick it in the relevant `ROADMAP.md` |

A falsified approach is worth as much as a fix and is the thing most often lost.
Write down what killed it, not just that it died.
