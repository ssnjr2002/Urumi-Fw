# Core 0 layers: primitives, ops, controller

Split Core 0 into explicit layers so the controller can grow on the Pico
without losing the atomic, config-free command set the host-as-controller
design produced. Follows docs/plans/pico-config.md, whose Open questions this
takes over.

## Decisions

* Layout, everything Core 0 runs lives under `core0/`:

  ```
  core0/
    core0.cpp  control_plane.*  data_plane.*  status.*  usb_protocol.h   loop + front door
    config/       config store, decoder, active MachineCfg
    ops/          operations: everything that writes machine state
    cmd/          primitive commands: parse, gate, reply; call ops/
    controller/
      seq/        sequences built from ops (default map; later home/probe recipes)
      cmd/        controller commands: parse, reply; call seq/
  ```

* Dependencies point down only: `cmd/` → `ops/`; `controller/cmd/` →
  `controller/seq/` → `ops/` + `config/`.
* Primitive commands are atomic and config-free: everything they need is an
  argument. Controller commands read the config.
* Only ops write the state machine (`machineState`, `alarmReason`). The
  controller changes state only by calling ops.
* `ops/` = code that writes state or holds a session: the position model, the
  axis-map commit, the state settle (`resumeOrHold`), the homing and probe
  supervisors. Reads (`query.cpp`) and single-RPC relays (`periph.cpp`) stay
  in `cmd/` and call `ipc/` directly.
* Dispatch is two tables. `control_plane` looks up the primitive table, then
  the controller table. The controller table has one gate: a valid config,
  else `err unconfigured`.

## Branch 1: `refactor/core0-layers`

**Type:** refactor. Same behaviour; existing tests pass unchanged.

**Purpose:** the layout and dispatch above, with behaviour unchanged.

**Files:**

1. Moves (one commit), includes updated:
   * `src/rp2350/config/*` → `src/rp2350/core0/config/`
   * `src/rp2350/controller/controller.*` → `src/rp2350/core0/controller/seq/`
   * `src/rp2350/core0/{position,homing,probe}.*` → `src/rp2350/core0/ops/`
   * `test/test_config/test_config_decode.cpp`: path to `config_decode.cpp`.
2. `ops/axis_map.*`: `axisMapApply`, `axisMapComplete`, `axisMapRetry`,
   `axisMapGate`, `axisNodeInConfig` out of `cmd/axis.cpp` (`:189-300`);
   `cmdAxisMap` stays and calls them. `cmd/axis_map.h` is removed.
3. `ops/state.*`: `resumeOrHold()` out of `homing.cpp:63`.
   `controllerApplyDefaultMap` (`controller.cpp:12-17`) stops writing
   `ALARM_CONFIG` and calls `resumeOrHold()`, which yields the same state.
4. Two tables: `control_plane.cpp:14-56` gains a controller table;
   `controller/cmd/` holds its handlers. `unalarm` moves there from
   `cmd/lifecycle.cpp:74`: without a config the machine is always in
   `ALARM_CONFIG`, where `unalarm` already answers `err unconfigured`, so the
   gate changes nothing.
5. Path references in comments and docs (`docs/homing.md`, `tool_probe.md`,
   `node_state_ingest.md`, `node_frame_ownership_migration.md`,
   `PLAN_rp2350_refactor.md`, `include/common.h`, `ipc/shared_state.h`, web
   comments in `sim.ts`, `status.ts`, `blob.ts`, `controller.test.ts`).

**Out of scope** (behaviour changes, see Later): anything that changes a
reply, a state or a gate.

**Known exceptions left in place:** `cmd/query.cpp` (`status cfg`) and
`cmd/axis.cpp` (`axis_map`'s `err unconfigured` / `not_in_config`) still
include the config; `axes_enable`, `setorigin`, `resumeOrHold()` and the soft
reset still check `ALARM_CONFIG`.

**Checks:** `pio run -e pico`, `pio test -e native`, `pnpm typecheck` and
`pnpm test` (comment-only web edits).

**Overlap:** none (`platformio.ini` needed no change).

**Depends on:** nothing.

**Status:** merged (c0be523). Not yet run on hardware. Unblocks the Later
items. `pio run -e pico` passes; `pio test -e native`
`test_parity` fails for missing untracked reference files (unrelated);
no web code changed, so the web checks were not run.

**Outcome:**

* One reply change, agreed: after `stop` on an unconfigured machine the
  reason is `ALARM_ESTOP`, and `unalarm` now answers `err unconfigured` (the
  gate) instead of `err unmapped`.
* `resumeOrHold()` writes the reason, a `__dmb()`, then the state, matching
  the controller's inline write it replaced.
* `ops/` still reads the config: `resumeOrHold()` and the axis-map ops
  (`axisMapComplete/Retry`, `axisNodeInConfig`). Another known exception
  until the feature branch.
* `unalarm` keeps its inline `ALARM_CONFIG` check; the gate makes it
  redundant with a valid config. Drop it in the feature branch.
* Path references were updated only inside `src/`. Follow-up: `docs/`
  (`homing.md`, `tool_probe.md`, `node_state_ingest.md`,
  `node_frame_ownership_migration.md`, `PLAN_rp2350_refactor.md`),
  `include/common.h`, and web comments (`sim.ts`, `status.ts`, `blob.ts`,
  `controller.test.ts`).

## Later

Moved to docs/plans/state-handling.md.
