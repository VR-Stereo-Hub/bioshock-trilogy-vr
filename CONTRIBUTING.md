# Contributing

This repo has more than one author now, plus coding agents working on behalf of each of
us. Everything below exists so that two people can work at the same time without
standing on each other, and so that anything that lands on `main` has been looked at by
someone who did not write it.

If you are an agent: read this file before your first commit of a session. The rules
here override any habit of committing straight to the checked-out branch.

## The one rule

**Never commit or push to `main` or `staging`.** Not a hotfix, not a one-line doc typo,
not "it is obviously safe". Both move only through a merged pull request.

`staging` is the day-to-day integration branch: work that is tested but not yet ready for
a release. Your feature branches PR into `staging`; `main` receives a single
`staging` -> `main` PR when a release is ready, and the tag is cut from `main` after it.

The reason is not ceremony. `staging` is what both of us branch from, and `main` is what
releases are cut from. A direct push rewrites the ground under whatever the other person
branched an hour ago, and the conflict shows up later, in someone else's unrelated work,
where it is expensive to untangle. Going through a PR is also how you *find out* there is
a conflict at all, while it is still cheap to fix and re-test.

## The loop

```bash
git checkout staging
git pull
git checkout -b <type>/<short-topic>
# ... work, commit, commit ...
git push -u origin <type>/<short-topic>
gh pr create --base staging --draft
```

Then: review, address comments, merge, delete the branch.

**Every PR targets `staging`.** Never open one from a feature branch into another
feature branch: stacked PRs get dirty fast, and the second one's diff is unreadable
until the first lands. If your work sits on top of a branch that has not merged yet,
wait for it to reach `staging` and rebase onto it.

Open the PR **at every good stopping point** - a feature that works and has been tested -
rather than saving it all for the end. Small and frequent is what lets the other person
join a subject while it is still in flight, and it is how conflicts stay small. A large
roll-up PR is for the end of a cycle, not the norm.

Releases are their own step: a `staging` -> `main` PR, then the tag is cut from `main`
after that merge, never from a branch.

### Branch names

`<type>/<short-topic>`, lowercase, hyphenated, no dates and no ticket numbers.

| Type | For |
|---|---|
| `feat/` | new player-visible behaviour |
| `fix/` | a defect in shipped behaviour |
| `docs/` | documentation only, no code |
| `build/` | CMake, submodules, packaging, CI |
| `tools/` | anything under `tools/`, including the simulator |
| `chore/` | version bumps, dependency pins, housekeeping |

Agents may keep their existing `claude/<topic>-<hash>` form; the hash suffix is there to
stop two parallel agent sessions from colliding on a name, which is a real problem the
type prefixes do not solve.

One branch is one subject. If, halfway through, you find a second unrelated thing worth
fixing, write it down and open a second branch for it. A branch that grows a second
subject stops being reviewable, which is the whole point of the exercise.

### Keep your branch fresh

If `staging` moves while you are working, bring it in rather than letting the gap grow:

```bash
git fetch origin
git rebase origin/staging
```

Rebase while the branch is yours alone and unpushed, or pushed but not yet under review.
Once someone has started reviewing it, use `git merge origin/staging` instead: a rebase
rewrites the commits the reviewer has already read, and their comments end up attached
to hashes that no longer exist.

## Commits

One logical change per commit. This is the part that makes review possible, and it is
worth the extra minute.

The test: could this commit be reverted on its own, and would that leave a tree that
still builds and still makes sense? A commit that mixes an aim fix with a rename and a
doc update fails that test, and a reviewer has to either take all three or argue about
all three at once.

Format is plain conventional commits, imperative, subject 72 characters or less, no
trailers:

```
fix(bs1): stop the viewmodel drifting after a weapon swap
feat(core): per-weapon aim profiles keyed on the resolved FName
docs(bs1): record the square deadzone measurement
```

Scope is the game (`bs1`, `bs2`, `bsi`), `core`, `tools`, or omitted.

The body is for **why**, not what. The diff already says what. If a number in the change
came from a measurement, the body is where the measurement goes, and it is the single
most useful thing you can leave for the person reading this in six months.

Do not commit game-derived content. That rule is in `CLAUDE.md` under Hard rules and it
is absolute.

## Pull requests

**Open the PR early, as a draft.** A draft PR is how the other person finds out that a
subject is already being worked on. Discovering that twice, at merge time, is the
failure this whole document is trying to prevent.

A PR description should answer four things:

1. **What changes**, in a sentence a player would understand.
2. **Why**, including the measurement or the report that prompted it.
3. **How it was verified.** Name it precisely: which simulator script, or which headset
   run and what you looked at. "Tested" on its own is not a verification claim.
4. **What was not verified.** Every PR has some of this. Saying so is what lets the
   reviewer aim their attention, and it is never held against you.

Keep it small. A PR that touches thirty files gets a worse review than three PRs that
touch ten, because the reviewer's attention does not scale with the diff.

### Review

Both of us review each other's work, and neither of us merges our own PR without at
least one look from the other. That is the only approval gate, and it applies to code.
A docs-only PR can be self-merged when it is genuinely just prose.

What review is looking for, roughly in order:

- Does it hook, scan or write memory anywhere, and does it fail closed if the target is
  not what it expected?
- Does a BS1 path change behaviour? BS1 is the headset-accepted baseline and regressions
  there cost a headset session just to notice.
- Is a number hardcoded that should have been derived, or copied between games? The Hard
  rules in `CLAUDE.md` forbid the second one outright.
- Is the verification claim in the description actually supported?

Comments are about the change, not the author. "This writes `Controller.Rotation` during
a scripted sequence, which we know breaks landings" is useful. Approve when it is good
enough to ship, not when it is perfect; the follow-up branch is cheap.

### Merging

Merge commits, matching the existing history (`merge: v0.8.2 - BS1 load-save crash fix`).
Do not squash: the individual commits are the reviewable unit and squashing throws away
the reasoning in their bodies. Delete the branch after merge; it is recoverable from the
PR forever.

If the PR reports a conflict, fix it **on the branch and re-test it there** before
merging. A conflict resolved blind at merge time is how a tested feature stops being one.

## Releases

A release is a `staging` -> `main` PR, then the tag is cut from `main` after that merge,
never from a branch. Version bump, release notes and tag are their own `chore/` PR so that
the tag lands on a reviewed commit.

## Working notes for agents

- **Never merge without the user confirming it first.** Committing and opening PRs is
  yours to do - that is what the flow above is for, and an open or draft PR changes
  nothing until it is merged. The merge is the irreversible step: show what is about to
  land and wait for a yes. Finishing a feature is not permission to merge it.
- **Branch first, before the first edit.** The most common agent failure here is making
  three good commits on `staging` and only then noticing. Check `git branch --show-current`
  before you write anything.
- **Do not push someone else's branch,** including one an agent created in a previous
  session, unless you are explicitly continuing that work.
- **Do not force-push a branch that has an open PR** with review comments on it.
- Read `docs/STATUS.md` and the relevant `docs/<game>/ENGINE_NOTES.md` before touching
  engine internals. The session protocol in `CLAUDE.md` is not optional.
- **Validate in the simulator before asking a human for a headset session**
  (`tools/xrsim-*.ps1`, catalog in `docs/VERIFICATION.md`). Headset time is the scarcest
  resource on this project.
- End the session by updating `docs/STATUS.md` and pushing. A session that ends without
  pushing STATUS.md is a failed handoff.

## Reporting a defect

Open an issue rather than a PR if you do not have the fix. Include
`%LOCALAPPDATA%\BioshockVR\bioshockvr.log` from the run that showed the problem, your
headset and runtime (VDXR, SteamVR, other), and what you were doing when it happened.
The log is what makes a report diagnosable; without it almost nothing can be acted on.
