# Repo and organisation practices

`CONTRIBUTING.md` covers what a contributor does. This file covers what the repository
and the `VR-Stereo-Hub` organisation should be configured to do, because a convention
that is only written down gets broken by accident, and one that GitHub enforces does not.

Most of the items below need **admin** on the repo or the org. Written 2026-08-21,
against the settings live at that date.

## What is not set today

Checked 2026-08-21 against the live repo:

| Setting | Now | Should be |
|---|---|---|
| Rulesets / branch protection on `main` | **none** | require a PR, block force-push |
| Delete branch on merge | off | on |
| CI build check | none | required on every PR |
| `CODEOWNERS` | absent | present |
| Discussions | off | fine as is; issues are enough for now |
| Licence | MIT | unchanged, but see *Incoming code* below |

`main` being unprotected is the one that matters. Every rule in `CONTRIBUTING.md` is
currently honour-based, and the failure it guards against (a direct push landing under
someone else's in-flight branch) is exactly the kind that happens by accident at 1am.

## Protecting `main`

Settings -> Rules -> Rulesets, targeting `main`:

- **Require a pull request before merging**, 1 approval.
- **Dismiss stale approvals when new commits are pushed.** Without this, an approval
  given to one diff silently carries over to a completely different one.
- **Block force pushes** and **restrict deletions**. Non-negotiable on a shared branch.
- **Require branches to be up to date before merging**, once there is a CI check worth
  gating on. Before that it only adds rebases.
- **Require status checks to pass**: the build (see below).

**Give org admins a bypass.** With two or three people this is a small team, and a rule
with no escape hatch turns a 2am release-blocking typo into a hostage situation. The
bypass exists to be used rarely and visibly, not to be routine: GitHub logs every use.

Also turn on **automatically delete head branches** (Settings -> General). The branch
list is already 29 entries deep and most of them are merged work.

## A build check is worth more than any review rule

One GitHub Actions workflow that configures and builds `Release | Win32` on every PR
catches the single most common wasted round trip: a change that is fine in review and
does not compile. It needs a Windows runner, the submodules, and about five minutes.

```yaml
# .github/workflows/build.yml
on: [pull_request]
jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
        with: { submodules: recursive }
      - run: cmake --preset win32
      - run: cmake --build build --config RelWithDebInfo
```

Make it a required status check once it is green on `main`. It says nothing about
whether the mod works in a headset, and it is not meant to. It says the tree builds,
which is the cheapest useful thing to know and the most annoying to discover late.

If a second check is ever added, the simulator selftest (`tools/xrsim-selftest.ps1`) is
the candidate: it needs no headset and no GPU, and it fails loudly when the runtime
plumbing breaks.

## CODEOWNERS

A `.github/CODEOWNERS` file auto-requests review from the right person and, combined
with the ruleset, can require it. With per-game adapters this maps cleanly onto the tree:

```
*                       @mohamad-balouza
/src/game/bioshock1r/   @mohamad-balouza @BioVRDev
/docs/bioshock1/        @mohamad-balouza @BioVRDev
```

Start permissive. CODEOWNERS that demands review from someone who is asleep is how a
two-person project learns to bypass its own rules.

## Issues and labels

Issues are on, which is right: a public mod gets bug reports and they are the main
source of what to fix next. Worth having, and no more than this:

- `bug`, `enhancement`, `docs`
- one label per game: `bs1`, `bs2`, `bsi`
- `needs-log` - the single most common reason a report cannot be acted on
- `needs-headset` - work that is blocked on someone putting the headset on, which is
  the real bottleneck and deserves to be visible

The bug template in `.github/ISSUE_TEMPLATE/` asks for the log and the runtime up front,
which is what turns a report into something diagnosable.

## Releases

Tag on `main` only, after the merge that contains the version bump. The tag, the release
notes and the zip should describe the same commit, and the only way to guarantee that is
to cut all three from `main` after the fact rather than from the branch that did the work.

Attach the built zip to a GitHub Release rather than pointing users at a build artifact.
Artifacts expire; releases do not, and a bug report against "the version I downloaded in
August" needs that version to still exist.

## Across the organisation

`VR-Stereo-Hub` holds five mods. Where they share shape, they should share practice: the
same `CONTRIBUTING.md`, the same PR template, the same protection ruleset, the same
`bug` / `enhancement` / `needs-log` labels. An org-level ruleset can target every repo at
once, which is less work than five copies and does not drift.

The thing that does **not** transfer between them is engine knowledge. That is per-game
and belongs in each repo's `ENGINE_NOTES.md`, and the Hard rules in `CLAUDE.md` already
forbid copying a number between games in the same tree, let alone between repos.

## Incoming code from another repository

Before code moves in from an outside tree, check what licence it currently carries.
Contributing into an MIT repo relicenses the work as MIT, and a source repo with **no
LICENSE file at all** is not MIT by default, it is all-rights-reserved. That has to be
settled by the author in writing before the first line moves, not after.

This applies to the in-flight consolidation with `Bioshock-Remastered-VR`, which has no
LICENSE file as of 2026-08-21. The fix is one commit in that repo, and it should land
before the first port PR opens.
