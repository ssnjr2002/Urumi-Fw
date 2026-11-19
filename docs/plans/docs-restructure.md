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
  plans/         one plan per change, kept; its status line says open or closed
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
* **Write for a reader new to the subject,** not as a terse densely packed
  summary for one who already knows it. Settled in branch 8.
* **Write a page for the pages that exist.** A page doesn't take on
  material because the page planned to hold it isn't written yet; it links
  or leaves it out. Settled in branch 8.

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

## Conventions: reference pages

Settled on `reference/commands.md` (branch 2).

* **Subject:** one kind of exact string (commands, status keys, packets,
  config fields), every one of them, one row each, grouped by use.
* **Checked against a commit.** The page names the commit whose code it was
  checked against, and the `git diff` that shows what changed since. Updating
  the page means re-checking the diff and moving the commit. No test.
* **Second words get their own table:** a command whose second word picks a
  form (`home`, `leg`, `goto`, `place`) leaves its group's table for a
  section after it: what every form shares, then one row per form.
* **Shared answers once:** what many rows share (reply shapes, node results,
  when a kind is admitted) is a table at the top; rows name it.
* **Links to the physical page** for an activity, not a copy of it.

## Conventions: logical pages

Settled on `system/controller.md` (branch 3).

* **Subject:** one layer: what it owns, who writes each value, and what
  crosses its edges, up and down.
* **Checked against a commit**, as reference pages are, with a `git diff`
  over the layer's files.
* **Code named by file**, never by line.
* **Decisions** at the end of the page: each one with the alternatives it
  rejected. Settles decision records for logical pages.
* **Decisions read as a chain:** each follows from the one before, in
  threads with no headings; a new thread starts where the chain breaks.
  The order carries the reasoning, with no "so" linking them. Settled in
  branch 9.

* **Reads in order:** no section leans on a term a later one introduces. A
  short overview names the parts before the sections that detail them.
* **One subject per section;** a section that holds several splits.
* **Code names by notation:** a value or function is named in words with its
  code name at first use in each section, by its code name after (README,
  Notation). Who writes what lists every name, so it doubles as the glossary.
* **Plain headings:** I/O, not Edges. Each I/O boundary is named by its
  ends: "Host to Core 0", not "Up, to the host".

Section order:

1. **Intro**, under the title: what the layer is, in a few sentences.
2. **Hardware:** what the layer runs on and touches.
3. **I/O:** what crosses each boundary, and over which channel.
4. **Lifecycle:** boot, the overview of the parts, failures and how the
   layer recovers, teardown. Failures sit with the faults that report them,
   not in a section of their own.
5. **The layer's own sections**, each a part the overview named.
6. **Who writes what:** a table of value, writer, readers.
7. **Decisions.**
8. **Code:** the files.

## Conventions: plans

Settled in branch 3.

* **A plan stays in `plans/`** whatever its state; moving it breaks its
  links. `archive/` holds docs from eras the project has left, not plans.
* **Open or closed:** a plan is open while any branch is neither merged nor
  dropped, closed once every branch is. Its first lines say which.
* **Replaced by a page:** a plan whose decisions a `system/` page now
  describes opens with a line naming the page: "Current behaviour:
  `system/<page>.md`; this plan records how it was built."

## Not settled

* Decision records outside logical pages.
* Status words for decisions.
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

   **Status:** done, merged at `f5f1a08`. Unblocks branch 2.

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
2. `refactor/command-reference`: `docs/reference/commands.md`, every
   controller command, marked with the commit it was checked against.

   Depends on 1.

   **Plan:**

   * The command words live in two arrays in
     `src/rp2350/core0/control_plane.cpp`: `kCommands` (primitives, lines
     20–73) and `kControllerCommands` (refused without a config, lines
     75–91), 58 words. A row is the word, an optional second word
     (`goto probe`, `goto mount`), the handler and a kind: `CmdKind`
     (`src/rp2350/core0/ops/machine.h:97`) with its `K_IN_*` and
     `K_BARE_READ` flags, admitted by `machineAdmit`
     (`src/rp2350/core0/ops/machine.cpp:178`). Forms and replies are parsed in
     the handlers, not the table.
   * `docs/wire_protocol.md`'s text-command table lacks 19 words (`cfg`,
     `nodepos`, `nodestat`, `vac_switch`, `reset`, `rst`, the five
     peripherals, `step`, `hallscan`, `axes_enable`, `bus_enable`,
     `probe_map`, `probe_end`, `setprobe`, `unprobe`), and its allowed-state
     matrix predates contexts and kinds.
   * `docs/reference/commands.md` (new): one row per table row, grouped by use
     (reads, lifecycle, bus and nodes, peripherals, motion primitives,
     homing, probe, positioning, tools, jobs, config). Columns: Command,
     Forms, Replies, Runs, Does. Runs names the kind (`any`, `read`,
     `write`, `step`, `sequence`, `context`, `clear`), its flags (`homing`,
     `probe`, `tool wait`, `place`, `bare read`) and `needs config` for the
     controller table, then any narrower check the handler adds; the kinds
     are defined once at the top. `uncfg` is marked as built only with
     `PICO_ALLOW_UNCONFIGURED`. Everything is checked against the handlers
     by hand.
   * No test. The page names the commit whose code it was checked against;
     a reader diffs `control_plane.cpp` and the handlers from there to see
     what may have drifted.
   * `docs/wire_protocol.md`: the text-command table and the allowed-state
     matrix stay, under a note that `reference/commands.md` supersedes them.
     The `get` keys stay as they are.
   * `src/rp2350/core0/control_plane.cpp`: the comments at lines 1 and 125
     point to `docs/reference/commands.md`.
   * `docs/README.md`: a `reference/` row in the folders table.
   * This doc: reference page conventions move from "Not settled" to their
     own Conventions section.
   * Checks: `pio run -e pico`.

   **Status:** done, merged at `ae46941`. Unblocks branch 3.

   **Outcome:**

   * Planned as `feature/` with a test that checked the page against the
     command tables. The test was dropped for the checked-against commit
     (`ecd2b3c`), and with no new behaviour left the branch was renamed
     `refactor/`.
   * `home`, `leg`, `goto` and `place` each have a section of their own, one
     row per form, rather than one crowded row; recorded in the reference
     conventions.
   * The second comment in `control_plane.cpp` was at line 113, not 125.
   * Out of scope, for a later pass:
     * `wire_protocol.md` says a bare `enable` or `disable` reaches every
       node; both need an id and answer `err bad_node` without one.
     * The comment above `cmdStep` (`src/rp2350/core0/cmd/axis.cpp`) says
       the count is clamped; it is refused with `err too_many`.
     * The `get` keys are still only in `wire_protocol.md`; they are the
       next reference page.

### The logical layer

One `system/` page per layer. Branch 3 writes the first and settles the
logical page conventions; branches 4–14 each write one more page and depend
only on branch 3, so they can run in parallel sessions, except branch 7,
which waits for branch 12. Each branch that lands a page moves the
top-level docs it replaces to `archive/`, with a line in
`archive/README.md`; the plans it replaces stay and get the line from
"Conventions: plans". It also adds the
page to `docs/README.md` (the first one also adds the `system/` row). A
`docs/README.md` conflict between parallel branches is one line each.
`wire_protocol.md` stays: it becomes `reference/` pages, not a `system/`
page.

| Branch | Page | Covers | Code | Replaces |
| --- | --- | --- | --- | --- |
| 3 | `controller` | how Core 0 runs the machine: contexts, sequences and steps; faults and the e-stop latch; the published state; command kinds and admission; the one state writer | `core0/ops/machine.*`, `sequence.*`, `runner.*`, `control_plane.cpp`, `core0.cpp` | `plans/machine-model.md`, `sequences.md`, `state-handling.md`, `core0-layers.md` |
| 4 | `cores` | Core 0 and Core 1: the three channels (RPC, shared state, the planner ring), facts, the e-stop sweep, the flash park | `ipc/`, `core1/core1.cpp`, `rpc_server.cpp` | `node_frame_ownership_migration.md`, `node_state_ingest.md` |
| 5 | `bus` | RS485: 9-bit framing, stream bytes and command packets, slots and `ENGAGE`, `axes_map`, keepalive and silence, make safe | `core1/bus/`, `node/rs485/`, `ops/slot_map.*`, `ops/axes_map.*` | `engage_and_axis_map.md` |
| 6 | `planner` | bezier to trapezoid to the ring to steps; the mesh | `lib/planner/`, `core0/planner/`, `core1/emit/` | `generic_planner_firmware_design.md` |
| 7 | `nodes` | node firmware: the core and type split, the hooks, the stepper, vacuum and knife types, legs on the node, the hall index | `src/node/` | `node_type_architecture.md`, `homing.md` §1, the architecture half of `src/node/README.md` |
| 8 | `jobs` | the job stream and its headers, layers, placement, tool change, pause, cancel and abort | `data_plane.cpp`, `controller/seq/place.*`, `toolchange.*`, `web/src/toolpath/job.ts` | the job sections of `wire_protocol.md` and the `job-*` plans |
| 9 | `config` | jsonc, validation, the msgpack blob, the store, the decode to `MachineCfg`; `set` and `save` | `core0/config/`, `web/src/machine/` | `config_storage.md` |
| 10 | `host` | the link (reader, writer, sessions, demux), the backends and the Sim, the machine model | `web/src/wire/link/`, `web/src/index.ts` | `comms_architecture.md` |
| 11 | `toolpath` | an SVG to BEZIER records: load, layers and tools, clean, annotate, the job's packets | `web/src/svg/`, `web/src/toolpath/`, `web/src/wire/format/bezier.ts`, `layer.ts` | nothing live |
| 12 | `firmware-build` | `platformio.ini`, the Arduino cores and libraries, the Pico SDK, upload | `platformio.ini`, `scripts/pio/`, `src/rp2350/main.cpp`, `uart_9bit.pio` | the `platformio.ini` half of `src/node/README.md` |
| 13 | `host-build` | the `web/` package: pnpm, TypeScript and its two configs, ESM exports, eslint, vitest, the vite demo, the generator scripts | `web/package.json`, `tsconfig*.json`, `vitest.config.ts`, `eslint.config.js`, `web/scripts/`, `web/demo/` | the Development section of `web/README.md` |
| 14 | `tests` | the firmware suites (`native`, doctest, stubs) and the host suites (vitest), and the fixtures that cross between them | `test/`, `web/test/` | `test/README` |

Every branch is `refactor/` (no behaviour change). Checks: none for
docs-only branches; a branch that edits code comments runs the check for
that code (see `AGENTS.md`, Agent Scope).

3. `refactor/system-controller`: `docs/system/controller.md`, how Core 0
   runs the machine.

   Depends on 2.

   **Plan:**

   * Covers: contexts, sequences and steps (the levels); the fault mask and
     the e-stop latch; the published state; command kinds and admission; the
     one state writer; teardown. `reference/commands.md`'s "When a command
     runs" links here.
   * Code: `src/rp2350/core0/ops/machine.*`, `sequence.*`, `runner.*`,
     `src/rp2350/core0/control_plane.cpp`, `core0.cpp`,
     `src/rp2350/ipc/shared_state.h` (`MachineState`).
   * Replaces the plans `plans/machine-model.md` (open: M4),
     `plans/sequences.md`, `plans/state-handling.md`,
     `plans/core0-layers.md` (closed). They stay in `plans/` and get the
     line from "Conventions: plans", with open or closed.
   * `machine/homing.md` and `machine/positioning.md` link the page in their
     Underneath.
   * This doc: the logical page conventions above lose "Proposed".

   **Status:** merged. Unblocks branches 4–11, 13 and 14; branch 7 also
   waits for 12.

   **Outcome:**

   * Plans are not archived: they stay in `plans/`, open or closed, with the
     "Current behaviour" line ("Conventions: plans"). `machine-model.md` is
     open (M4); the other three are closed.
   * The logical page conventions changed on review: Hardware and I/O first,
     Failures folded into Lifecycle's faults, the owns table moved late as
     "Who writes what", and the reads-in-order rule.
   * `machine/homing.md`'s node-reset line was wrong and is fixed: the datum
     witness catches a reset at the node's next bind or leg end.
   * Comments that disagree with the code, left for a code branch:
     * `ipc/shared_state.h`: the `MachineState` comment omits the
       sequence-by-name rule.
     * `ops/machine.h`: `K_WRITE` says "at rest", narrower than the code.
     * `controller/cmd/unalarm.cpp` cites `engage_and_axis_map.md` §6.1,
       which branch 5 archives.
   * For branch 5: `F_BUS` comes only from the boot sweep, so a bus outage
     after boot raises no fault; the silence timeout and the keep-alive are
     the page's to cover. `state-handling.md`'s open items (SET_SESSION, the
     silence timeout, a node watchdog) go to branches 5 and 7; its
     `alarmReason` two-writers item is stale.
   * For branch 7: the shelved `feature/park-speed` branch touches
     `docs/homing.md`.
   * `machine-model.md` said `home` runs in `JOB.PAUSED`; the code needs an
     IDLE context. That is M4's to settle.

4. `refactor/system-cores`: `docs/system/cores.md`, the split between Core 0
   and Core 1.

   Depends on 3.

   * Covers: the three channels (RPC, shared state, the planner ring),
     facts, the e-stop sweep, the flash park, which core owns which value.
   * Code: `src/rp2350/ipc/`, `src/rp2350/core1/core1.cpp`,
     `core1/rpc_server.cpp`, `src/rp2350/main.cpp`.
   * Replaces `node_frame_ownership_migration.md`, `node_state_ingest.md`:
     proposals, never built, so they move to `plans/` as
     `node-frame-ownership.md` and `node-state-ingest.md`, each with a line
     saying why; the page links the first from its Decisions.

   **Status:** merged at `da3d1cb`. Unblocks nothing new: branches 5–11, 13 and 14 already wait only on branch 3.

   **Outcome:**

   * The two proposals were never built, so they moved to `plans/`
     (`node-frame-ownership.md`, `node-state-ingest.md`) rather than
     `archive/`, each with a line saying why. Their decisions stay in them;
     the page links only the first.
   * A new logical-page convention, code names by notation (README, Notation),
     applied to `cores.md` and retrofitted to `controller.md`. Later pages
     follow it.
   * `controller.md` keeps its "Core 0 to Core 1" and links `cores.md`.
   * `bus` and `planner` are named in plain text, not linked; branches 5 and
     6 can link them when their pages land.
   * Out of scope, for a later pass:
     * Stale comments: `ipc/core1_rpc.h` narrates the old FIFO encoding and
       cites `control_plane.cpp:405` and "§8.2"; `ipc/shared_state.h` keeps
       the "split out of shared.h" history, "state_redesign Layer 4", and
       points `BUS_ADDR_MAX` at `engage_and_axis_map.md` §9;
       `core1/core1.cpp` has the flash-park comment above `busKeepalive()`
       instead of `core1FlashPark()`; `core1/emit/emit.h` narrates its split
       from `shared.h`.
     * `node-state-ingest.md` §7 still names `alarmReason` and
       `MSEG_FLAG_ESTOP` as open items; both are gone from the code.

5. `refactor/system-bus`: `docs/system/bus.md`, RS485.

   Depends on 3.

   * Covers: 9-bit framing, stream bytes and command packets, slots and
     `ENGAGE`, `slot_map` and `axes_map`, keepalive and bus silence, make
     safe, node results.
   * Code: `src/rp2350/core1/bus/`, `src/node/rs485/`,
     `src/rp2350/core0/ops/slot_map.*`, `ops/axes_map.*`, `ops/bus.*`,
     `include/common.h`.
   * Replaces `engage_and_axis_map.md`.

   **Status:** merged at `7d81bf0`, from `refactor/bus` (not `refactor/system-bus`).
   Unblocks nothing new.

   **Outcome:**

   * The page is ordered from the wire up: Hardware, Data layer, Stream
     bytes, Command frames, then I/O and Lifecycle, so neither leans on a
     term introduced later. The settled order puts I/O and Lifecycle second
     and third; on a page whose parts are protocol layers they read only
     after the layers.
   * Stream bytes split into without reply (steppers) and with reply (the
     probe's vacuum, and the bed-level node when it joins): one reply per
     byte, lockstep, under half the bandwidth. Pure, pure reply and hybrid
     mixes, each with the probe leg that uses it.
   * Commands named on the page give their argument syntax and read-back
     marks before the text leans on them (`slot_map`, `axes_map`).
   * `engage_and_axis_map.md` moved to `archive/`, under a new "Replaced by
     system pages" section. Links in live docs and code comments point at
     `system/bus.md`; the two that cite its §9 (the type-blind relay verbs,
     which the page does not cover) point at the archived copy. `cores.md`
     now links the page.
   * Left out as no longer true: the `ALARM_*` reasons (now `F_BUS`,
     `F_UNMAPPED`), the FIFO-word engage, the staging tasklist, and the
     claim that ingest refuses a step for an unbound axis, which no check in
     the code makes.
   * Out of scope, for a later pass:
     * Plans that name `engage_and_axis_map.md` (`state-handling.md`,
       `motion-sessions.md`, `pico-config.md`) keep the old name, as records.
     * The archived doc links `tool_probe.md` relatively; that link is
       broken, as the archive README allows.
     * No node build enables `NODE_HAS_SILENCE_TIMEOUT`; the page says so.

6. `refactor/system-planner`: `docs/system/planner.md`, from curve to step.

   Depends on 3.

   * Covers: bezier and line records, the trapezoid, the planner queue and
     ring, the follower on Core 1, the mesh.
   * Code: `lib/planner/`, `src/rp2350/core0/planner/`,
     `src/rp2350/core1/emit/`, `src/rp2350/core0/ops/mesh.*`.
   * Replaces `generic_planner_firmware_design.md`.

   **Status:** merged at `6903b18`. Unblocks nothing new.

   **Outcome:**

   * The page covers more than the plan named: Z and A lines, A following
     a tangent curve, the blade offset, events and front pushes, and duty
     limits (tool events, Core 0's planned stop, Core 1's deadline). These
     were built under `planner-za.md` and `job-a.md`.
   * Ordered as a pipeline, from a move coming in to the stream byte going
     out, as the bus page is ordered from the wire up: I/O and Lifecycle
     come after the pipeline's sections. The intro names the mesh and duty
     limits, so earlier sections can point forward to them.
   * Added a Codebase section after Hardware: the three layers (the
     platform-free library, Core 0's side, Core 1's side) and the tests.
     Code at the end is only a file table naming each file's sections.
     Proposed for other code-heavy layers, not made a convention.
   * Decisions are grouped into the library, the cores and motion.
   * Job framing (records, layers, swivel choice, lifts, buffering, pause
     and resume) is left to branch 8's page. This page names only the
     hooks jobs use: events, tags, front pushes, the job states the
     follower sets.
   * `generic_planner_firmware_design.md` moved to `archive/`. The five
     "seed §N" comments (`follower.cpp`, `bezier.h`, `planner.h`,
     `trapezoid.h`) name the archived copy. `pico-planner.md` is open
     (branch 3 deferred), `planner-za.md` closed, and `job-a.md` closed,
     its line pointing here only for A's following and limits.
   * Checks: `pio run -e pico`, `pio test -e native` (207 cases) pass.
   * Out of scope, for a later pass:
     * `job-a.md` needs the jobs page for its job side (branch 8).
     * Tool config paths are named by code name (`maxOnS`), since the
       config's tool section isn't settled in the page's notation.
     * `pico-planner.md` branch 5c still says "awaiting merge"; its code
       is on `main`.

7. `refactor/system-nodes`: `docs/system/nodes.md`, the node firmware.

   Depends on 3 and 12.

   * Covers: the core and type split, the hooks, the stepper, vacuum and
     knife types, the board headers, legs on the node, the hall index, the
     two dispatch paths.
   * Code: `src/node/`.
   * Replaces `node_type_architecture.md`, `docs/homing.md` §1 (the node
     half), and the architecture half of `src/node/README.md`. Branch 12
     takes the README's `platformio.ini` half; this branch moves the rest
     to `archive/node_readme.md`.
   * Overlap: the shelved `feature/park-speed` edits `docs/homing.md`.
     Archiving it conflicts with that branch when it is picked up.
   * Added in Write: `docs/homing.md` stays, with §1 cut to a pointer to the
     page, since §2 and §3 are still live; the node source's comments are
     pruned, and their `homing.md` §1 references point to the page.

   **Status:** merged at `23d41af`. Unblocks no branch.

   **Outcome:**

   * The stepper part splits in four: the shared counter, DIR and pulse
     (§6), stream motion (§7), node-driven motion (§8), the Hall index (§9).
   * The page links `bus.md` for frames, slots and the status rather than
     restating them. "Two dispatch paths" covers both pairs: commands in
     `loop()` against stream bytes in the ISR, and the bus against the
     console, which share `routeCommand()`.
   * `src/node/README.md` is now two links; the old one is
     `archive/node_readme.md`. The archive renames landed in the
     `refactor(node)` commit, not the `docs:` one.
   * `include/common.h` was pruned too, to the wire layouts and one line per
     value (364 lines to 163). Only comments changed in any code file.
   * Checks: every node env and `pico` build without warnings;
     `pio test -e native` passed 207/207.
   * Out of scope, for a later pass:
     * The DB32 vacuum board sets the probe switch pin as a plain input, with
       no pull-up from the chip. Whether a cut wire reads open there, which
       the probe's fail-safe needs, is not known.
     * The vacuum and knife opcodes (0x10–0x14) sit in the range the old
       comments called generic (0x01–0x1F).
     * `web/src/machine/schema.ts` still names `node_type_architecture.md`
       twice.
     * The "adding a node type" steps went to the archive with the old
       README; no live page has them.

8. `refactor/system-jobs`: `docs/system/jobs.md`, a job from host to cut.

   Depends on 3.

   * Covers: the job stream and its headers, layers, placement, tool
     change, pause, cancel and abort. How the records are made from an SVG
     is branch 11's; this page starts at the stream.
   * Code: `src/rp2350/core0/data_plane.cpp` (its job half),
     `src/rp2350/core0/ops/layer.*`, `src/rp2350/core0/planner/queue.cpp`
     (its job half), `src/rp2350/core0/controller/seq/place.*`,
     `toolchange.*`, `src/rp2350/core0/cmd/lifecycle.cpp`,
     `web/src/wire/link/session.ts`, `web/src/wire/format/layer.ts`.
     (Was `web/src/toolpath/job.ts`, which is branch 11's.)
   * Replaces the `job-*` plans' decisions; which plans get the line from
     "Conventions: plans" is settled in the branch's Read.

   **Status:** merged at `5227523`. Unblocks nothing new.

   **Outcome:**

   * The Code list was corrected in Read (above). The job plan with most of
     the decisions is `motion-sessions.md`, which also holds jogs and
     status; its line points here for jobs only, and it stays open (S2
     planned). `job-z.md` and `job-session-web.md` are closed with the line;
     `job-a.md`'s line now points here for A in a job.
   * Stream flow control (seq, ACK, Go-Back-N, `seqreset`, the abort
     barrier's mechanism) is left to the host page (branch 10). This page
     covers why a job sends each packet, the job-only NACKs and the holds
     they cause.
   * Ordered as the job's stages first (preflight through the host's
     session), then the reference sections (glossary, hardware and code,
     stream, states, I/O, who writes what, decisions, code). Proposed for
     pipeline or sequence layers, with the planner page; not made a
     convention.
   * Two rules settled here, added to "Conventions: all pages": write for a
     reader new to the subject, and write for the pages that exist.
   * The code differs from `motion-sessions.md`, and the page follows the
     code: a job doesn't end on its last record (the host sends `cancel`);
     `JOB_HDR`'s job id and record count are unread; a dry ring doesn't lift
     (`LIFT_DRY` reserved); there is no `hostTimeoutMin` and no vacuum in
     jobs; `cancel` goes straight to IDLE; a tool change turns A to 0 only
     on a head whose holder locks.
   * Out of scope, for a later pass:
     * Nothing tests the firmware's job path (data plane, `plannerQueueRecord()`);
       the web job tests run against the Sim's reimplementation.
     * A lost host leaves the tool down and on mid-contour, with no timeout.
       Deferred, not dropped: `job-z.md` (What lifts: "lost host … later";
       ZB Out), `hostTimeoutMin` unbuilt (`motion-sessions.md`, JH2
       Outcome), `LIFT_DRY` reserved for it. The controller can't tell a
       slow host from a lost one without the timeout.
     * A job ending on its last record was planned but never built: how the
       record count bounds a job is open (`motion-sessions.md`, Job records),
       and JH1 left it out of scope, so a job ends by `cancel`.
     * Comments in `place.h`, `controller/cmd/place.cpp`, `toolchange.h`,
       `tools.h`, `follower.cpp`, `shared_state.h` and `schema.ts` still cite
       the plans; the plans stay in place, so the links hold.

9. `refactor/system-config`: `docs/system/config.md`, the config path.

   Depends on 3.

   * Covers: `config/controller.jsonc`, validation, the msgpack blob, the
     store, the decode to `MachineCfg`, runtime edits (`set`, `save`),
     `uploadfs`.
   * Code: `src/rp2350/core0/config/`, `web/src/machine/`,
     `scripts/pio/config_image.py`.
   * Replaces `config_storage.md`.
   * Added in Write: the comments in `web/src/machine/` and
     `src/rp2350/core0/config/` are pruned, and references to
     `config_storage.md` point to the page.

   **Status:** ready to merge.

   **Outcome:**

   * The page is ordered by the config's forms: Design, JSONC, the blob on
     the host, the wire, the blob on the controller, the runtime struct. The
     design section is a chain of decisions, now a logical-page convention.
   * The wire section names the steps and NACK reasons; byte layouts stay in
     `wire_protocol.md`. The old page's state table (IDLE or ALARM) was stale:
     a push needs at rest in the idle context, faults allowed.
   * `config_storage.md` moved to `archive/`. `pico-config.md` is closed and
     `runtime-config.md` open (branch 4, the web panel, not started); both
     have the "Current behaviour" line.
   * Comments pruned (about 610 lines to 230); only comments changed.
   * Checks: `pnpm typecheck`, `pnpm test` (664 passed), `pio run -e pico`
     without warnings, `pio test -e native` (207/207).
   * Out of scope, for a later pass:
     * `AGENTS.md` (Controller Config) should point to
       `docs/system/config.md` instead of `docs/config_storage.md`.
     * `uploadfs` runs `parseConfig`, not `loadConfig`
       (`web/src/machine/json/image.ts`): a file that breaks a host rule is
       flashed, caught only by the controller's decode at boot.
     * The web Sim has no config store (`pico-config.md`, deferred).
     * The host's `probe.seekFeed` rule compares with the editable `maxFeed`
       and may be dead (`runtime-config.md`).
     * `save` with nothing dirty still writes flash and moves `seq`.

10. `refactor/system-host`: `docs/system/host.md`, the web host.

    Depends on 3.

    * Covers: the link (reader, writer, sessions, demux), the backends
      (WebSerial, node, the Sim), the machine model.
    * Code: `web/src/wire/link/`, `web/src/index.ts`.
    * Replaces `comms_architecture.md`.

    **Status:** not started.

11. `refactor/system-toolpath`: `docs/system/toolpath.md`, an SVG to BEZIER
    records.

    Depends on 3.

    * Covers: loading an SVG (the transform walk, mm with Y up, layers and
      tool layers), cleaning subpaths (joins, gaps), annotating curves
      (start, break and end flags, refused curves), and the layer's packets
      (`prepareBezierJob`). What the controller does with them is branch 8's.
    * Code: `web/src/svg/load.ts`, `web/src/toolpath/`,
      `web/src/wire/format/bezier.ts`, `layer.ts`.
    * Replaces nothing live: the earlier designs are already in `archive/`
      (`PLAN_svg_tile_motion.md`, `planner_spaces.md`).

    **Status:** not started.

12. `refactor/system-firmware-build`: `docs/system/firmware-build.md`, how
    the firmware is built.

    Depends on 3.

    * Covers:
      * `platformio.ini`: the MCU templates (`attiny_base`,
        `attiny3226_base`, `avr128db_base`), `node_core`, the type sections,
        how a node env composes them (interpolation, not `extends`), the
        build flags, the `pico` env (PIO program, filesystem size,
        `extra_scripts`, `custom_config_json`), `native`, the scratch envs,
        and the `f_cpu` caveat.
      * Arduino cores: megaTinyCore (ATtiny), DxCore (AVR128DB),
        Earle Philhower's arduino-pico through the maxgerhardt platform
        (RP2350).
      * Libraries: TMCStepper, Servo and SPI on the nodes; ArduinoJson and
        LittleFS on the controller.
      * The Pico SDK under arduino-pico: what the controller uses directly
        (`hardware/sync`, `hardware/pio`, `hardware/gpio`,
        `hardware/clocks`, `pico/util/queue`) and the second core.
      * Upload: `serialupdi`, `picotool`, `uploadfs`.
    * Code: `platformio.ini`, `scripts/pio/`, `src/rp2350/main.cpp`,
      `src/rp2350/core1/bus/uart_9bit.pio`, `src/node/board/`.
    * Replaces the `platformio.ini` half of `src/node/README.md` (its env
      table is out of date: no vacuum or knife envs). `AGENTS.md` keeps its
      flag lists.
    * Also prunes `platformio.ini`'s comments, added in Write.

    **Status:** merged at `37f160b`. Unblocks branch 7.

    **Outcome:**

    * Ordered as the stack, bottom up: a short history of the build file,
      then silicon and boards, Arduino cores, SDK and libraries, our source,
      and PlatformIO (upload folded in). Not the stages-first order of the
      jobs page: a build isn't a pipeline. Board history and pinouts are left
      to a physical page; only the build's own history is told.
    * Scope widened: `platformio.ini`'s comments were pruned to one-line
      board facts, with the rest on the page, and each section was
      reformatted. Every env resolves as before (`pio project config`
      compared against `dd2cc69`). `board_build.pio` was removed: the
      platform never read it, and assembles every `.pio` under `src/` by
      itself.
    * `src/node/README.md` lost its `platformio.ini` section for a link to
      the page; the rest stays for branch 7.
    * Checks: `pio run` for `pico`, `node1`, `vac_node7`, `db_node1`,
      `knife_node8`, `vac_db_node7_dbg`, `hello_serial1`; `pio test -e
      native` (207 cases) pass.
    * Out of scope, for a later pass:
      * `AGENTS.md` calls `PICO_ALLOW_UNCONFIGURED` off by default; the
        `pico` env sets it.
      * The ATtiny envs (`node1`–`node4`, `vac_node7`) and their templates
        remain though the nodes have moved to the DB32.
      * Envs hardcode COM ports from one machine.
      * `uart_9bit.pio.h` is committed and rewritten by any build after the
        `.pio` changes.

13. `refactor/system-host-build`: `docs/system/host-build.md`, how the host
    is built.

    Depends on 3.

    * Covers:
      * pnpm: `web/package.json`, `pnpm-workspace.yaml`, the lockfile, Node
        18 or later.
      * TypeScript: `tsconfig.json` (typecheck) and `tsconfig.build.json`
        (`dist/`), ESM, the package's `exports` (the root, the WebSerial
        and Node backends).
      * Dependencies: `@msgpack/msgpack`; the dev tools (vitest, vite,
        vite-node, eslint, `@xmldom/xmldom` for SVG in Node).
      * Scripts: `build`, `typecheck`, `lint`, `test`, `gen:runtime`,
        `demo`; and `web/scripts/` (`config-image.ts`, `mesh-image.ts`,
        `runtime-fields.ts`), with what runs them (`uploadfs` runs
        `config-image.ts` through `scripts/pio/config_image.py`).
      * The demo: `web/demo/`, its vite config and its bridge.
    * Code: `web/package.json`, `web/tsconfig*.json`,
      `web/vitest.config.ts`, `web/eslint.config.js`, `web/scripts/`,
      `web/demo/`.
    * Replaces the Development section of `web/README.md`, which stays as
      the package's README and links here.

    **Status:** not started.

14. `refactor/system-tests`: `docs/system/tests.md`, the firmware and host
    tests.

    Depends on 3.

    * One page for both, since their fixtures cross: the web tests write
      the config blobs (`web/test/fixtures/config/`) and the BEZIER records
      (`test/data/bezier_records.txt`, `GEN_CPP_REF=1`) that the C++ suites
      read.
    * Covers:
      * Firmware: the `native` env, doctest, `test/main.cpp`, the stubs
        (`test/stubs/`), each suite and what it checks, running one suite.
      * Host: vitest, `web/test/setup.ts`, the helpers and test machines,
        the suites by folder, the production data.
      * What neither covers: the Human Scope list in `AGENTS.md`.
    * Code: `test/`, `web/test/`, `web/vitest.config.ts`.
    * Replaces `test/README`.

    **Status:** not started.
