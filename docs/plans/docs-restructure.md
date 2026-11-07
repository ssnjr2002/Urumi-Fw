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

Proposed; settled on `system/controller.md` (branch 3).

* **Subject:** one layer: what it owns, who writes each value, and what
  crosses its edges, up and down.
* **Checked against a commit**, as reference pages are, with a `git diff`
  over the layer's files.
* **Code named by file**, never by line.
* **Decisions** at the end of the page: each one with the alternatives it
  rejected. Settles decision records for logical pages.

Section order:

1. **What it is.**
2. **What it owns:** a table of value, writer, readers.
3. **Edges:** what crosses each one, and over which channel.
4. **States and lifecycle.**
5. **Failures.**
6. **Decisions.**
7. **Code:** the files.

## Not settled

* Decision records outside logical pages.
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
which waits for branch 12. Each branch that lands a page moves what the page
replaces to `archive/`, with a line in `archive/README.md`, and adds the
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
   * Replaces the merged plans `plans/machine-model.md`,
     `plans/sequences.md`, `plans/state-handling.md`,
     `plans/core0-layers.md`: moved to `archive/`.
   * This doc: the logical page conventions above lose "Proposed".

   **Status:** not started.

4. `refactor/system-cores`: `docs/system/cores.md`, the split between Core 0
   and Core 1.

   Depends on 3.

   * Covers: the three channels (RPC, shared state, the planner ring),
     facts, the e-stop sweep, the flash park, which core owns which value.
   * Code: `src/rp2350/ipc/`, `src/rp2350/core1/core1.cpp`,
     `core1/rpc_server.cpp`, `src/rp2350/main.cpp`.
   * Replaces `node_frame_ownership_migration.md`, `node_state_ingest.md`
     (proposals, not built: their decisions go in the page's Decisions).

   **Status:** not started.

5. `refactor/system-bus`: `docs/system/bus.md`, RS485.

   Depends on 3.

   * Covers: 9-bit framing, stream bytes and command packets, slots and
     `ENGAGE`, `slot_map` and `axes_map`, keepalive and bus silence, make
     safe, node results.
   * Code: `src/rp2350/core1/bus/`, `src/node/rs485/`,
     `src/rp2350/core0/ops/slot_map.*`, `ops/axes_map.*`, `ops/bus.*`,
     `include/common.h`.
   * Replaces `engage_and_axis_map.md`.

   **Status:** not started.

6. `refactor/system-planner`: `docs/system/planner.md`, from curve to step.

   Depends on 3.

   * Covers: bezier and line records, the trapezoid, the planner queue and
     ring, the follower on Core 1, the mesh.
   * Code: `lib/planner/`, `src/rp2350/core0/planner/`,
     `src/rp2350/core1/emit/`, `src/rp2350/core0/ops/mesh.*`.
   * Replaces `generic_planner_firmware_design.md`.

   **Status:** not started.

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

   **Status:** not started.

8. `refactor/system-jobs`: `docs/system/jobs.md`, a job from host to cut.

   Depends on 3.

   * Covers: the job stream and its headers, layers, placement, tool
     change, pause, cancel and abort. How the records are made from an SVG
     is branch 11's; this page starts at the stream.
   * Code: `src/rp2350/core0/data_plane.cpp`,
     `src/rp2350/core0/controller/seq/place.*`, `toolchange.*`,
     `web/src/toolpath/job.ts`.
   * Replaces the done `job-*` plans' decisions; which plans move to
     `archive/` is settled in the branch's Read.

   **Status:** not started.

9. `refactor/system-config`: `docs/system/config.md`, the config path.

   Depends on 3.

   * Covers: `config/controller.jsonc`, validation, the msgpack blob, the
     store, the decode to `MachineCfg`, runtime edits (`set`, `save`),
     `uploadfs`.
   * Code: `src/rp2350/core0/config/`, `web/src/machine/`,
     `scripts/pio/config_image.py`.
   * Replaces `config_storage.md`.

   **Status:** not started.

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

    **Status:** not started.

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
