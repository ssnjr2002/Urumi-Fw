# Urumi Development Guide

Open-source firmware and host software for a flatbed CNC cutter. The repo
map is in [README.md](README.md); how the machine and the code work is in
[docs/README.md](docs/README.md). This file is how to work here.

`docs/` holds the facts: the envs and build flags, the layers, the commands,
the host API. Read the page for what you touch rather than relying on this
file or memory.

---

## Environment

* **PlatformIO:** `~/.platformio/penv/Scripts/pio.exe` on Windows. Name the
  env: plain `pio run` builds only the default.
* **Host:** Node 22.12+ and pnpm, run in `web/`. `pio run -e pico -t
  uploadfs` also needs them: it encodes `config/controller.jsonc` with the
  host's encoder.
* **Python:** with pyserial, for `scripts/node_console.py`, the bench scripts
  and the demo's serial bridge.
* **The machine:** the Pico enumerates as a USB serial port (COM8 on this
  bench); nodes flash over a serial UPDI adapter. Only one program can hold a
  port: close a serial monitor or the bridge before uploading.
* **`web/dist`** is built locally and gitignored; `pnpm demo:dist` serves the
  demo from it. Rebuild it (`pnpm build`) after a `web/` merge.

### Windows

* Edit files with the Edit/Write tools. Never rewrite a file through
  PowerShell `Get-Content`/`Set-Content` or `>`: Windows PowerShell 5.1 reads
  UTF-8 as the ANSI codepage and writes a BOM, which corrupts non-ASCII text
  (`platformio.ini` box-drawing characters, em dashes in comments).
  `Get-Content` also shows UTF-8 as mojibake (`â†’` for `→`) when the file is
  fine; pass `-Encoding utf8` to read it.
* If a script must write a file, use .NET with explicit UTF-8 without BOM:
  `[IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding($false)))`
* PowerShell 5.1 has no `&&`: use `A; if ($?) { B }`. Git Bash is also
  available and suits short scripted edits.
* For commit messages with more than one line, write the message to a file
  and pass it with `git commit -F <file>`; piping text into git from
  PowerShell adds a BOM to the message.
* "LF will be replaced by CRLF" warnings from git are expected and harmless.

#### Worktrees and `node_modules`

* Link `main`'s packages into a new worktree with a junction:
  `cmd /c mklink /J web\node_modules <main folder>\web\node_modules`.
* Before `git worktree remove`, delete the worktree's `web/node_modules`
  with `cmd /c rmdir /s /q ..\urumi-<short-name>\web\node_modules`.
  `rmdir /s` removes a junction without following it. Git for Windows
  follows one: with a junction to `main`'s, `git worktree remove` empties
  `main`'s `web/node_modules`, and with a `pnpm install`ed one it fails
  partway with "Directory not empty".
* Run `rmdir` through `cmd /c`: in PowerShell, `rmdir` is an alias for
  `Remove-Item`, which is a different command.
* Check `git diff --stat` after any scripted edit. A line count far larger
  than the edit means the encoding was damaged: restore the file with
  `git checkout -- <file>` and redo the edit.


---

## Coding Standards

### Comment Style

* Keep comments short and write them for the merged state, as if the code
  had always worked this way.
* Remove before/after narration, investigation measurements, and rationale
  that belongs in the commit message.
* Keep only non-obvious mechanism, field/parameter meaning, or the reason a
  special case exists.

### Host API

The public surface of the host library, `urumi-host` (`web/src`), as its
subpaths export it and `docs/reference/host-api/` describes it.

* Keep it small: export what a user needs, nothing for convenience.
* One word per thing. Settle the vocabulary before writing code, and use the
  same word in code, docs and UI.
* The demo (`web/demo`) imports only the library's public subpaths. If it
  needs something internal, the API is missing it.

---

## Docs

The docs are part of the change. How a page is written is in
[docs/STYLE.md](docs/STYLE.md), and the notation in
[docs/README.md](docs/README.md).

* **Where a page goes:** `machine/` for what you see the machine do,
  `system/` for a layer underneath, `reference/` for exact strings, `plans/`
  for a change.
* **Stamps:** a `system/` or `reference/` page names the commit it was
  checked against. Updating it means reading the `git diff` it gives, then
  moving the stamp. Never move a stamp on a page you didn't check.
* **Write for the merged state,** for a reader new to the subject, and only
  for pages that exist: link or leave out, don't fill one page with another's
  material.
* **Docs travel with their code,** in the same branch. There is no docs-only
  branch type.

---

## Session and Git Workflow

The main folder always has `main` checked out. `main` is never checked out in
a worktree. Every other branch is worked on in its own worktree.

### Session Models

A plan doc is required whenever a change has more than one branch. A
one-branch change may be planned in chat.

#### Single Session

For one branch, or several branches that depend on each other and can't be
worked on in parallel.

* One session cycles through the states, one branch at a time:
  Start → Planning → Work (branch 1) → Merge → Planning → Work (branch 2) → …
* Each branch is created only when its turn comes, from `main` as it is after
  the previous branch merged.
* If the session grows long, the user may compact it or continue in a fresh
  session. The plan doc carries the context: "read docs/plans/<name>.md,
  continue from branch <N>".

#### Multiple Sessions

For branches that don't depend on each other.

* One Planning session writes the plan doc and commits it to `main`.
* Each independent branch gets its own Work session, run in parallel:
  "read docs/plans/<name>.md, do branch <N>".
* A dependent chain inside a larger plan runs as a Single Session alongside
  the parallel ones.
* The main-folder session merges each branch, in the Merge state, when its
  Work session hands back.

### Plan Docs

* Live in `docs/plans/<name>.md`, and stay there whatever their state; moving
  one breaks its links.
* Created only in a Planning session, and committed directly to `main` as
  `docs:`.
* **First lines say open or closed.** A plan is open while any branch is
  neither merged nor dropped, closed once every branch is.
* **The plan holds the details.** Vocabulary, decisions, drafts of reference
  entries and API sketches go in the plan first; the doc pages are written
  from it once the code is settled.
* **Replaced by a page:** once a `system/` page describes what a plan built,
  the plan opens with "Current behaviour: `system/<page>.md`; this plan
  records how it was built."
* Modified from any branch or folder. A Work session updates its own branch's
  section in its own branch, so the update merges with the code.
* One section per branch, holding that branch's:
  * Plan: name, type, purpose, files touched, doc pages touched, depends-on.
    Written in Planning.
  * Status
  * Outcome: appended before the branch merges: deviations from the plan,
    interface changes later branches rely on, findings out of scope. Keep it
    short; commit messages already record what changed and why.
* No shared status table; parallel branches then edit different lines and
  merge cleanly.
* On a merge conflict in a plan doc, deliberate with the user to resolve the
  conflicts before committing.

### Session States

```text
Start ──► Planning ──► Work ──► Merge ──► Planning ──► Work (next branch) …
              ▲          │        │
              └──────────┘        └──► Work (merge conflicts: rebase again)
            (plan wrong)
```

| State    | Location                   | Does                                              |
| -------- | -------------------------- | ------------------------------------------------- |
| Start    | wherever the session opens | works out where it is and which state to enter    |
| Planning | main folder, on `main`     | plans, writes and commits plan docs               |
| Work     | the branch's worktree      | implements the branch, prepares it for merge      |
| Merge    | main folder, on `main`     | merges a ready branch, cleans up                  |

#### Start

1. Find out where the session is:

   ```bash
   git branch --show-current
   git status --short
   git worktree list
   git remote -v
   ```

2. If the main folder is not on `main`, stop and tell the user.
3. If the working tree has changes this session didn't make, tell the user
   before doing anything else.
4. Decide the state from the user's request. If unclear, ask.
   * Planning or Merge:
     * Must be in the main folder. If the session is elsewhere, ask permission
       to move there.
   * Work:
     * If a plan doc exists for the task, read it. If there is none, ask the
       user what the branch should do, or whether to start in Planning.
     * If it is unclear which branch, ask.
     * If the branch already has a worktree, ask permission to move into it.
       Otherwise ask permission to create one from current `main` and move
       into it.

#### Planning

* Location: main folder, on `main`.
* Allowed: read code and docs, discuss, create and commit plan docs (after the
  user explicitly agrees).
* Not allowed: code edits.
* Cite the files and lines the plan touches, and the doc pages.
* Propose a branch type for each branch and let the user confirm it (see
  Branching).
* If a plan mixes types, split it into separate branches; refactors land first.

#### Work

* Location: the branch's worktree.
* Allowed: implement the branch, update this branch's section of the plan doc,
  commit when told.
* Stay within the branch's plan. Out-of-scope findings go in the Outcome, not
  into the code.
* Small plan changes happen here: if the change stays within this branch
  (same purpose, no effect on other branches), agree it with the user, update
  this branch's Plan section, and note it in the Outcome.

Work has two phases.

**Read**

* Read the code the branch touches, and the doc pages that describe it,
  before writing any. Where a page and the code disagree, that is a finding.
* No code edits. The plan may be written if it helps and the user agrees.
* End with a findings brief, then wait for the user's go-ahead:
  * What the code showed that the plan and docs didn't know.
  * Decisions that need the user (see below).
  * A one-line direction to start in. No step list.

**Write**

1. **Vocabulary first:** settle the names the branch adds or changes with the
   user, and record them in the plan.
2. **Code:** implement the branch. The details the docs will need collect in
   the plan as they are settled; the pages themselves are written with the
   Outcome (see Work to Merge). A reference page may be written before the
   code instead, when the user asks for it; the code is then made to match it.

* Decisions that need the user: scope changes, new files or modules,
  interface or behaviour changes, new dependencies, anything that affects
  another branch. If one comes up while writing, pause and ask then; don't
  defer it to the hand-back.
* Implementation details the agent settles alone go in the Outcome.
* After each coherent unit, with its Agent Scope checks passing, propose a
  commit with its message and wait. If the plan was updated in Read,
  propose that as its own commit before the first code edit.

#### Merge

* Location: main folder, on `main`.
* Allowed: merge a branch whose Work session handed it back as ready, when the
  user says so; remove its worktree.
* Not allowed: code edits, including resolving conflicts.

#### Planning to Work Transition

1. The user agrees to the plan.
2. If there is a plan doc, commit it to `main`.
3. Check other open `feature/`, `fix/` and `refactor/` branches for
   overlapping files:
   `git diff --name-only main...<other-branch>`
   Report overlap to the user, especially in the files branches tend to
   share: `platformio.ini`, `src/rp2350/core0/cmd/table.h`, `lib/planner/`,
   `web/src/packets/`, `web/src/config/`, and each `docs/reference/` page.
4. For the next branch in this session: create its worktree from current
   `main`, move the session into it, and continue as Work.
   For independent branches: give the user the opening message for each one's
   Work session; they can all start now. Branches in a dependent chain start
   one at a time, each after its dependency merges.

#### Work to Merge Transition

When the branch is finished, in the Work session:

1. Rebase onto current `main` (ask first; see Branching). Resolve conflicts
   here; for a plan doc conflict, deliberate with the user first.
2. Propose commit groups (see Merging); rewrite only once the user approves,
   and verify the tree is unchanged.
3. Run the Agent Scope checks. Any failure is named, with a follow-up.
4. Write the docs: update the pages the branch changed, from the plan, and
   move their stamps to the branch's commit. Propose the commit and wait.
5. Write the Outcome in the plan doc, set the branch's Status to `merged`,
   and name which branches it unblocks (or summarise it in chat if there is
   no plan doc). Commit when told. This is the branch's last commit: with a
   fast-forward merge it lands on `main` as the merge happens, so the merge
   adds no commit of its own.
6. Hand back: tell the user the branch is ready to merge. A parallel Work
   session ends here; in Single Session, move to the main folder.

A `bench/` branch stops after step 5: its Outcome is committed to `main`
instead of the branch, and there is no merge. Remove its worktree when done;
the branch stays.

#### Merge to Planning Transition

1. Merge when the user says so (see Branching), then rebuild `web/dist` if
   the branch touched `web/`.
2. Remove the worktree, in this order:
   1. The session is in the main folder, not the worktree; a shell whose
      directory is inside it keeps it locked.
   2. Stop anything served from the worktree (`pnpm demo`, the bridge).
   3. Delete the worktree's `web/node_modules` first, if it exists, without
      following links (on Windows, see Environment, Windows).
   4. `git worktree remove ../urumi-<short-name>`. If it fails, stop and
      tell the user; don't force it.
3. Delete the branch only if the user asks.
4. Single Session with branches remaining: re-read the plan doc, since the
   merged branch's Outcome may change the next branch's plan, then go back
   through Planning to Work.

#### Merge to Work Transition

If `git merge --ff-only` refuses, `main` moved after the branch's rebase.
Send the branch back to its worktree to rebase onto current `main`, then
return through Work to Merge.

#### Work to Planning Transition

When the plan turns out to be wrong mid-branch in a way that changes the
branch's purpose or type, or affects other branches:

1. Stop coding. Record what was found in this branch's section of the plan doc.
2. Tell the user and ask whether to replan. Replanning happens in the main
   folder; the branch and worktree stay as they are until the plan is settled.

### Branching

* `main` is the only long-lived branch. Branch from it, rebase onto it, merge
  into it. (Exception: `release/X.Y` hotfix branches, see Tags.)
* Create a branch only when work on it starts, from current `main`.
* Never push or force-push. Finish the local work, then stop and give the
  user the exact command.
* Never rewrite history (rebase, amend, filter) on a branch without asking
  first. If a rewritten branch then needs pushing, give the user a
  `--force-with-lease=<branch>:<expected-sha>` command, never plain `--force`.
* If a push is rejected after a history rewrite, do NOT pull; that merges the
  old history back. Tell the user it needs a force-with-lease push.
* Never open, close or merge a PR; that is the user's.

#### Branch Types

* `feature/`: new behaviour
* `fix/`: wrong behaviour made right, with a test that fails before the fix
  where one is practical
* `refactor/`: same behaviour, different structure; existing tests should pass
  unchanged
* `bench/`: scratch firmware or scripts that measure something on the bench.
  Code lives in `src/scratch/` and its own scratch env, or in
  `scripts/`, and touches no production code. Commits use `chore(bench)`. A
  bench branch never merges: its numbers go in the plan doc's Outcome,
  committed to `main` as `docs:` (or in chat, without a plan doc), and the
  branch stays for re-running.

#### Branch Naming Convention

```text
feature/<short-description>       # New features
fix/<issue-number>-<description>  # Bug fixes
refactor/<component-name>         # Code refactoring
bench/<what-is-measured>          # Bench measurements
```

**Examples**:

- `feature/tool-duty-limits`
- `fix/soft-limits`
- `refactor/config-storage`
- `bench/planner`

#### Merging

Merge from the main folder on `main`. The branch doesn't need to be checked
out; it can stay in its worktree.

Before merging, in the Work session, rebase onto `main` and group the commits:

* One commit per `type(scope)`. Scopeless commits group by type.
* Plan doc and Outcome commits stay separate from other `docs:` commits.
* Groups keep the order of their first commit.
* If moving a commit into its group conflicts, leave it as its own commit
  and tell the user.
* The agent proposes the groups; the user approves before the rewrite.
* Keep the old tip until the merge:
  `git branch backup/<branch> <old-tip>`.
* Verify: `git diff backup/<branch> <branch>` must be empty. If any groups
  touch the same files, run each group's Agent Scope checks on its commit.

Merge as a fast-forward: `git merge --ff-only <branch>`. If it refuses,
`main` moved after the rebase; see Merge to Work Transition.

* Merge order: dependencies first; refactors and protocol changes before
  features; the smaller of two overlapping branches first.
* A known failing check may merge only if it is named, with a follow-up.
* After the merge, delete `backup/<branch>`.

### Commits

#### Commit Message Format

**Pattern**:

```text
<type>: <short summary (50 chars max)>

<optional detailed description>
```

**Types**: `feat`, `fix`, `refactor`, `docs`, `test`, `chore`, `perf`

**Example**:

```text
feat: add tool duty limits

Implements a cooldown for the tool for the defined period.

Tested the happy path and it works.
```

* A scope is optional: `feat(probe): add touch-off leg`.
  Common scopes: `node`, `pico`, `web`, `planner`, `probe`, `homing`, `wire`.
* Every commit an agent makes ends with a trailer naming the model:
  `Co-Authored-By: <Model name> <noreply email>`, e.g.
  `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`

#### When to Commit

**DO commit when**:

- User explicitly requests: "commit these changes"

**DO NOT commit when**:

- Build fails or has warnings
- Experimenting or debugging in progress
- User hasn't explicitly requested commit
- Files excluded by `.gitignore` would be included
- The staged diff contains scratch or bench output (CSV captures,
  `.bin`/`.plan`/`.png` outside the fixture directories)

**Rule**: **If uncertain, ASK before committing.**

#### Staging

* Never `git add -A` or `git add .`; stage named paths.
* Check `git status --short` before committing: a `git add` naming a path
  that no longer exists fails and stages nothing from that command.
* Untracked files are the user's decision: list them and ask.
* Ask before committing any single file over ~1 MB or ~5,000 lines.
* Test fixtures live in `web/test/fixtures/` or `web/test/production/data/`.
  Scratch firmware lives in `src/scratch/`, scripts in `scripts/`. Nothing
  new in the repo root (`config/` and the generated `data/` are the
  exceptions).

#### Direct Commits to `main`

Everything reaches `main` by merge, with two exceptions:

* `docs:` commits to `docs/plans/**`.
* Anything else the user explicitly asks to commit directly to `main`. The
  request covers that change only, not later ones.

### Tags

* Never create, move, delete or push a tag without being asked. Pushing a tag
  is the user's.
* `fw/vX.Y.Z` and `host/vX.Y.Z`: independent version lines, need to pass
  Agent and Human scope.
* `fw/vX.Y.Z-rc.N` / `host/vX.Y.Z-rc.N`: release candidates; need to pass
  Agent scope only. The final tag goes on the same commit once Human scope
  passes.
* `archive/<name>-<yyyy-mm>`: preserved snapshots. Never build on these.
* A hotfix for a shipped version: branch `release/X.Y` from its tag, fix it
  there (cherry-pick from `main` where possible), tag `X.Y.Z+1`. Nothing is
  merged back.

### Worktrees

* Each Work session has its own worktree on its own branch; never two
  sessions in one folder.
* The same branch can't be checked out in two worktrees at once, and `main`
  is never checked out in one.
* Create from `main`:
  `git worktree add ../urumi-<short-name> -b <type>/<short-name> main`
* A new worktree has no `web/node_modules` or `.pio/`. Either run
  `pnpm install` in `web/`, or link `main`'s `web/node_modules` into it.
  Either way, delete it before removing the worktree (see Merge to
  Planning).
* A worktree sees only committed files. Commit a plan doc before creating the
  worktrees that need it.

---

## Testing and Verification

Two scopes: what the agent checks, at the desk or on the machine, and what
only the user can judge. What each env, test env and script does is in
[firmware build](docs/system/firmware-build.md) and
[host build](docs/system/host-build.md).

### Agent Scope

Run the checks for what changed, once, after the last code edit:

| Changed                  | Run                                                    |
| ------------------------ | ------------------------------------------------------ |
| `src/node/`, node envs   | `pio run -e <each affected env>`                       |
| `src/rp2350/`            | `pio run -e pico`                                      |
| `lib/planner/`           | `pio run -e pico` and `pio test -e native`             |
| `web/`                   | `pnpm typecheck`, `pnpm lint` and `pnpm test` in `web/` |
| `platformio.ini`         | `pio run -e <each env whose section changed>`          |
| a `docs/` page           | its links resolve, and its stamp moved only if checked |

* `native` is a test env: use `pio test -e native`, not `pio run`.
* Report failures with their output; never describe a red check as passing.

#### On the machine

The agent may also drive the machine itself, once the user says it is
connected and is there to stop it.

* **Needs:** coordinates, soft limits, homing and the guards in place, and the
  e-stop within the user's reach. Without them, motion is Human scope.
* **Two ways in:**
  * the demo UI, through the serial bridge (`web/demo/bridge/`) and
    `shim.js`, when the browser has no port picker;
  * scripts: pyserial against the Pico's port, or `scripts/node_console.py`
    against a `_dbg` node.
* **Report** what was run and what the machine did, step by step, with any
  faults and their text. Never describe a step that wasn't run.
* **Leave it as found:** say what is mounted, homed and zeroed at the end,
  and stop the bridge so the port is free.
* The user is asked before anything that cuts, plunges or runs a tool's
  power.

### Human Scope

List these for the user instead of claiming them:

* What only a person can judge: cut quality, mechanical feel, noise, a tool's
  behaviour on material.
* Motion when the guards under Agent Scope, "On the machine", aren't in
  place.
* RC sign-off on the machine before a release tag.
