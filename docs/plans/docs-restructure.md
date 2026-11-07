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

Settled on `machine/homing.md` and `machine/positioning.md` (branch 1).

* **Subject:** an activity: something you start, watch and end (homing,
  positioning, tools, running a job). Parts (gantry, head, holder, bed) are
  the words every page uses, not pages. Things that happen to any activity
  (e-stop, pause, faults, limits) are rows in each page's refusals or
  recovery, not pages of their own.
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

* **Notation by link:** a page links the README's notation, not its own copy.

Section order:

1. **What it is.**
2. **The concepts**, each with the commands, replies and status keys that
   belong to it, and when those commands run. These sections are the page's
   own: cycles, legs and kinds for homing; coordinates, selection, zeroing,
   moves and limits for positioning.
3. **Commands:** an index table only (command, what it does, section), one
   row per form.
4. **Failures or refusals**, as a table.
5. **Recovery and stops**, with where it doesn't help.
6. **Underneath.**

A linear-and-rotary comparison sits with the concepts, wherever the page's
kinds differ.

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

   **Status:** ready to merge.

   **Outcome:**

   * Step 1 archived 28 docs, not just the Python-host and web-planner ones:
     also Pico-era designs never built or since rewritten (`bus_alarm.md`,
     `feed_override.md`, `coordinate_frames_and_limits.md`, the LittleFS
     migration, `validation_checklist.md`, the planner audit and spaces
     notes). `config_storage.md` stays: it describes the store as built.
     `docs/README.md` lists only the folders that exist.
   * Step 3 is `machine/positioning.md`: coordinates, head selection, manual
     zeroing (`wzero z` and `wzero z_switch`), jogs, `goto`, limits.
     Automatic zeroing and the bed mesh are left out; the holder lock,
     `mount` and `unmount` belong to a later tools page.
   * A fourth commit reshaped the homing page to the settled section order.
   * The homing page's Underneath links today's docs, not the `system/` and
     `reference/` pages, which don't exist yet.
   * Out of scope, for a later pass:
     * Code comments naming archived docs: `src/rp2350/core0/ops/position.h`
       (`bus_alarm.md`), `web/src/machine/schema.ts` and
       `web/src/machine/json/load.ts` (`feed_accel_value_model.md`,
       `coordinate_frames_and_limits.md`), `web/test/machines.ts`
       (`planner_audit.md`, `head_binding.md`).
     * Live docs naming archived ones in plain text: `comms_architecture.md`,
       `homing.md`, `PLAN_rp2350_refactor.md`, `node_state_ingest.md`, and the
       plans `job-z.md`, `planner-za.md`,
       `state-handling.md`, `motion-sessions.md`, `coordinate-system.md`,
       `pico-config.md`.
   * Not verified on the machine: whether the host's stop brakes the Z lift
     that starts a `goto` (a park leg, not a planner move).
     `machine/positioning.md` claims the stop only for a `goto`'s XY, Z and
     A moves.
2. `reference/commands.md` and a test that checks it against the controller's
   command tables. Changes code, so a branch of its own; type open. Depends
   on 1.

Each gets a full Plan section when its turn comes.
