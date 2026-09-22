# Urumi Controller Development Guide

Project: Open-source firmware and control software for a flatbed CNC cutter.

---

## Project Architecture

### Firmware

#### Build System: PlatformIO 

1. On Windows:
    * Location: ~/.platformio/penv/Scripts/pio.exe

**Configuration Files**:

* `platformio.ini`: Main build configuration

Contains config for 4 MCU types:

* Nodes:
    * ATtiny 3224: `attiny_base`
    * ATtiny 3226: `attiny3226_base`
    * AVR128DB32: `avr128db_base`
* Controller:
    * RP2350: `env:pico`
* Scratch: Miscellaneous envs used for testing specific things.

Node Types:

* Core (shared across all types): `node_core`
* Vacuum Controller Node: `type_vacuum`
* Knife Controller Node: `type_knife`
* Stepper Controller Nodes: `type_stepper` 

Node Environments:

* Combination of 
    * MCU
    * Node type 
    * Build Flags

Node Build Flags:

* All nodes:
    * `NODE_ID`: Specifies the node addressing id of a node.
    * `NODE_DEBUG_CONSOLE`: Address a node over USB Serial instead of RS485 Bus through a python script (node_console.py).
    * `NODE_HAS_PROBE_REPLY`: Allow a special stream reply mode which relays probe switch state. 
    * `RS485_USE_XDIR`: Turn on XDIR, an automatic hardware direction switch for half duplex RS485 buses. 
    * `NODE_HAS_LASER`: Node has a laser attached to it which can be toggled on or off.
* Stepper nodes:
    * `DM542`: Specify the stepper driver as a DM542 driver.
    * `TMC_2660`: Specify the stepper driver as a TMC 2660 driver.
    * `TMC_CURRENT`: Specify the current setting value for TMC 2660 driver.
    * `TMC_MICROSTEPPING`: Specify the microstepping value for TMC 2660 driver.
    * `DRV8825`: Specify the stepper driver as a DRV8825 driver.
    * `DRV_MICROSTEPPING`: Specify the microstepping value for DRV8825 driver.
    * `HAS_LIMIT_SWITCH`: Specify that a node has a limit switch and will support linear homing.
    * `LIMIT_ACTIVE_HIGH`: Specify if the limit switch signal is high when active.
    * `HAS_HALL_INDEX`: Specify that a node has hall effect sensor and may support rotary homing.
* Vacuum nodes:
    * `BOARD_DB32_VACUUM`: Specify that this board is a AVR128DB32 vacuum board.

### Software

In web/src

---

## Coding Standards

### Comment Style

* Keep comments short and write them for the merged state, as if the code had always worked this way.
* Remove before/after narration, investigation measurements, and rationale that belongs in the commit message.
* Keep only non-obvious mechanism, field/parameter meaning, or the reason a special case exists.

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
* If the session grows long, the user may compact the current session or continue in a fresh session. The
  plan doc carries the context: "read docs/plans/<name>.md, continue from
  branch <N>".

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

* Live in `docs/plans/<name>.md`.
* Created only in a Planning session, and committed directly to `main` as
  `docs:`.
* Modified from any branch or folder. A Work session updates its own branch's
  section in its own branch, so the update merges with the code.
* One section per branch, holding that branch's:
  * Plan: name, type, purpose, files touched, depends-on. Written in Planning.
  * Status
  * Outcome: appended before the branch merges: deviations from the plan,
    interface changes later branches rely on, findings out of scope. Keep it
    short; commit messages already record what changed and why.
* No shared status table; parallel branches then edit different lines and
  merge cleanly.
* On a merge conflict in a plan doc, deliberate with the user to resolve the conflicts before committing.

### Session States

```text
Start ──► Planning ──► Work ──► Merge ──► Planning ──► Work (next branch) …
              ▲          │        │
              └──────────┘        └──► Work (merge conflicts: rebase again)
            (plan wrong)
```

| State    | Location                   | Does                                              |
| -------- | -------------------------- | ------------------------------------------------- |
| Start    | wherever the session opens | works out where it is and which state to enter   |
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
* Allowed: read code, discuss, create and commit plan docs (after user explicitly agrees).
* Not allowed: code edits.
* Cite the files and lines the plan touches.
* Propose a branch type for each branch and let the user confirm it (see
  Branching).
* If a plan mixes types, split it into separate branches; refactors land first.
* Docs travel with the code they describe, in the same branch.

#### Work

* Location: the branch's worktree.
* Allowed: implement the branch, update this branch's section of the plan doc,
  commit when told.
* Stay within the branch's plan. Out-of-scope findings go in the Outcome, not
  into the code.
* Small plan changes happen here: if the change stays within this branch
  (same purpose, no effect on other branches), agree it with the user, update
  this branch's Plan section, and note it in the Outcome.

#### Merge

* Location: main folder, on `main`.
* Allowed: merge a branch whose Work session handed it back as ready, when the
  user says so; remove its worktree; update the plan doc's Status for that
  branch.
* Not allowed: code edits, including resolving code conflicts.
* Plan doc conflicts are the exception: deliberate with the user and resolve
  them here.

#### Planning to Work Transition

1. The user agrees to the plan.
2. If there is a plan doc, commit it to `main`.
3. Check other open branches for overlapping files:
   `git diff --name-only main...<other-branch>`
   Report overlap to the user, especially in: `platformio.ini`,
   `src/rp2350/core0/cmd/table.h`, `web/src/wire/`, `web/src/machine/schema.ts`,
   `lib/motion/`.
4. For the next branch in this session: create its worktree from current
   `main`, move the session into it, and continue as Work.
   For independent branches: give the user the opening message for each one's
   Work session; they can all start now. Branches in a dependent chain start
   one at a time, each after its dependency merges.

#### Work to Merge Transition

When the branch is finished, in the Work session:

1. Rebase onto current `main` (ask first; see Branching).
2. With 10 or more commits, propose commit groups; rewrite only once the user
   approves.
3. Run the Agent Scope checks. Any failure is named, with a follow-up.
4. Write the Outcome in the plan doc (or summarise it in chat if there is no
   plan doc), and commit when told.
5. Hand back: tell the user the branch is ready to merge. A parallel Work
   session ends here; in Single Session, move to the main folder.

#### Merge to Planning Transition

1. Merge when the user says so (see Branching).
2. Remove the worktree. Delete the branch only if the user asks.
3. Mark the branch done in the plan doc, and name which branches it unblocks.
4. Single Session with branches remaining: re-read the plan doc, since the
   merged branch's Outcome may change the next branch's plan, then go back
   through Planning to Work.

#### Merge to Work Transition

If the merge conflicts in code, `main` moved after the branch's rebase. Abort
the merge (`git merge --abort`) and send the branch back to its worktree to
rebase onto current `main`, then return through Work to Merge.

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

There is no docs-only branch type; docs go in the branch of the code they
describe.

#### Branch Naming Convention

```text
feature/<short-description>       # New features
fix/<issue-number>-<description>  # Bug fixes
refactor/<component-name>         # Code refactoring
```

**Examples**:

- `feature/tool-duty-limits`
- `fix/soft-limits`
- `refactor/config-storage`

#### Merging

Merge from the main folder on `main`. The branch doesn't need to be checked
out; it can stay in its worktree.

| Branch             | Before merging (in the Work session)                                                             | Merge as                 |
| ------------------ | ------------------------------------------------------------------------------------------------ | ------------------------ |
| Under 10 commits   | rebase onto `main`                                                                               | squash                   |
| 10 or more commits | rebase onto `main`, group into logical commits; agent proposes the groups, user approves first   | merge commit (`--no-ff`) |

* Merge order: dependencies first; refactors and protocol changes before
  features; the smaller of two overlapping branches first.
* A known failing check may merge only if it is named, with a follow-up.

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
* Untracked files are the user's decision: list them and ask.
* Ask before committing any single file over ~1 MB or ~5,000 lines.
* Test fixtures live in `web/test/fixtures/` or `web/test/production/data/`.
  Scratch code lives in `src/scratch/`. Nothing new in the repo root.

#### Direct Commits to `main`

Everything reaches `main` by merge, with two exceptions:

* `docs:` commits to `docs/plans/**`.
* Anything else the user explicitly asks to commit directly to `main`. The
  request covers that change only, not later ones.

### Tags

* Never create, move, delete or push a tag without being asked. Pushing a tag
  is the user's.
* `fw/vX.Y.Z` and `host/vX.Y.Z`: independent version lines, need to pass Agent and Human scope tests.
* `fw/vX.Y.Z-rc.N` / `host/vX.Y.Z-rc.N`: release candidates; needs to pass agent scope tests only. The final tag goes on the same commit once human scope passes.
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
* A new worktree has no `web/node_modules` or `.pio/`; run `pnpm install` in
  `web/` before the first test run.
* A worktree sees only committed files. Commit a plan doc before creating the
  worktrees that need it.
* After the branch is merged, remove its worktree with
  `git worktree remove ../urumi-<short-name>` (the branch and its commits are
  unaffected).

## Testing and Verification

### Agent Scope

Run the checks for what changed, once, after the last code edit:

| Changed                  | Run                                             |
| ------------------------ | ----------------------------------------------- |
| `src/node/`, node envs   | `pio run -e <each affected env>`                |
| `src/rp2350/`            | `pio run -e pico`                               |
| `lib/motion/`            | `pio run -e pico` and `pio test -e native`      |
| `web/`                   | `pnpm typecheck` and `pnpm test` in `web/`      |
| `platformio.ini`         | `pio run -e <each env whose section changed>`   |

* Plain `pio run` builds only the default env; name the envs.
* `native` is a test env: use `pio test -e native`, not `pio run`.
* Report failures with their output; never describe a red check as passing.

### Human Scope

List these for the user instead of claiming them:

* Motion, homing and probing on the machine
* Anything timing- or bus-dependent (RS485, step streams, node replies)
* RC testing on the machine before a release tag

---

## Agent Tooling on Windows

* Edit files with the Edit/Write tools. Never rewrite a file through
  PowerShell `Get-Content`/`Set-Content` or `>`: Windows PowerShell 5.1 reads
  UTF-8 as the ANSI codepage and writes a BOM, which corrupts non-ASCII text
  (`platformio.ini` box-drawing characters, em dashes in comments).
* If a script must write a file, use .NET with explicit UTF-8 without BOM:
  `[IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding($false)))`
* For commit messages with more than one line, write the message to a file
  that way and pass it with `git commit -F <file>`; piping text into git from
  PowerShell adds a BOM to the message.
* Check `git diff --stat` after any scripted edit. A line count far larger
  than the edit means the encoding was damaged: restore the file with
  `git checkout -- <file>` and redo the edit.

---