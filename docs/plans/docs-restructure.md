# Docs restructure

How the docs are laid out and written. The docs in `docs/` today are design notes,
plans and audits from three successive hosts (Python, the web planner, the Pico
controller); nothing in them is reference, and nothing describes the machine as
you see it. This plan holds the conventions agreed so far and the branches that
apply them.

## Findings

From an audit of the 59 docs in `docs/` and `docs/plans/` (2026-11-06):

* **No doc kinds.** Plans, transcripts, audits, references and wishlists look
  alike. Status is free prose in a dozen wordings.
* **Status lines rot apart from the body.** `wire_protocol.md` has 79 commits
  and still opens "Phase 1 contract FROZEN"; `homing.md`'s status names a file
  that is gone.
* **Supersession is one-way.** A newer doc names what it replaces; the older one
  doesn't know.
* **Plans became the reference.** Current behaviour is the sum of a plan's
  decisions minus the later ones that reversed them.
* **The reference is incomplete.** 16 of about 59 controller commands are not in
  `wire_protocol.md`; most live only in plans.
* **No way in.** No README or index; 16 docs have no inbound link.
* **Organised by software layer, not by the machine.** Planner, config and
  controller state have 7–11 docs each. The physical machine has none; vacuum,
  laser, knife, mesh, the two heads and e-stop are mentioned in 4–32 docs each
  and owned by none.
* **What lasted:** dated measurements (`rotary_a_axis.md`), procedures that say
  what a failure rules out (`validation_checklist.md`), decisions that keep their
  rejected alternatives, and contracts that must change with the code.

## Two layers

* **Physical:** what you see on the machine and what you should expect it to
  do. Stable across rewrites of the code underneath, though not frozen.
* **Logical:** what happens underneath: controller states, bus traffic, who
  owns which value, the bridges between host, controller and nodes. Changes
  as the code does.

Physical pages link down to logical ones; logical pages need not link up.

## Layout

Proposed; only `machine/` has been tried.

```text
docs/
  README.md      start here: the machine, a map of these folders, how to read them
  machine/       physical: one page per part or capability you can see
  system/        logical: one page per layer (bus, nodes, controller, planner, jobs, config, host)
  reference/     exact commands, replies, status keys, packets, config fields
  howto/         step-by-step tasks
  measurements/  dated bench results, each with date, firmware commit and hardware
  plans/         as now; disposable once their branches merge
  archive/       dead-era docs, frozen, with a README naming what replaced each
```

## Conventions: all pages

### Notation

Each string is written the way you meet it:

| Written as | Is |
| --- | --- |
| `home unhomed` | a command, as you type it |
| `→ err estop` | the controller's reply |
| `get homefail`, `→ homefail=1` | a status key: asked for with `get`, read as `key=value`; never on its own |
| `machine.x.homing.backoffDist` | a config field, as its full path in `config/controller.jsonc` (the JSON schema, not the Pico's structs) |
| `<node>` | a placeholder you fill in |

* In config paths, `<linear>` is any linear axis (`x`, `y` or `heads[].z`),
  `<rotary>` any rotary axis (`heads[].a`), and `<axis>` either:
  `machine.<linear>.homing.backoffDist`.
* No marks inside tables: the column header says what a cell is.
* No superscripts or bracketed marks; plain Markdown renders neither well.

### Content

* Write for the merged state, in the present tense. No branch or date header.
* No test status on a page that describes behaviour. What has been tried on the
  machine goes in `measurements/` or a plan's Outcome.
* Codes, replies and command forms are checked against the source before they
  are written down.

## Conventions: physical pages

Settled on the homing page (`machine/homing.md`, branch 1 step 2).

* **Subject:** a part or capability you can see: homing, a head, the knife, the
  probe, the vacuum, the bed, e-stop.
* **Nodes, controller and host are named by their role**, as you can tell them
  from what they are wired to: "the axis's node", "the controller". Never by
  config instance: no "head 0", "node 7".
* **Machine words:** the gantry, a head, a holder, a switch, a magnet.
* **Known gaps are behaviour:** what the machine does not protect you from,
  stated as plainly as what it does.
* **Comparisons go in tables** with one column per kind (linear and rotary
  side by side), not in parallel prose.
* **Failures as a table:** what you see, the code, the cause, and which kinds of
  axis it applies to.
* **Recovery says where it doesn't help.**
* **Ends with "Underneath":** links to the `system/` and `reference/` pages.

Section order on the homing page: what it is, commands, the sequence you see,
its steps, its kinds, failures, recovery. Not yet a template: a second page
decides which of these every page shares.

## Not settled

* Logical (`system/`) page conventions.
* Reference page conventions, and which pages a test checks against the code.
* Decision records: a log at the end of each page, or a `decisions/` folder.
* Status words for plans and decisions.
* Whether the operator is an audience (`howto/run-a-job.md`).

## Branches

The restructure is a one-off: after it, new pages go in the branch of the code
they describe, as `AGENTS.md` already requires. So it runs as one `refactor`
branch (same behaviour, different structure), with one commit per step for
review; no new branch type.

1. `refactor/docs`, in steps:
   1. Move the Python-host and web-planner docs to `archive/` with a README
      naming what replaced each; add `docs/README.md` with the layout and the
      notation.
   2. Move `docs/plans/docs-restructure/homing-draft.md`, written while
      settling these conventions, to `machine/homing.md`.
   3. A second machine page, to settle the template. Which part: open.

   Depends on nothing.
2. `reference/commands.md` and a test that checks it against the controller's
   command tables. Changes code, so a branch of its own; type open. Depends
   on 1.

Each gets a full Plan section when its turn comes.
