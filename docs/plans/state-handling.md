# State handling: config, bus, sessions, alarm exits

Where the Pico's state machine goes after docs/plans/core0-layers.md. That plan
settled the layers (primitives in `cmd/`, state writes in `ops/`, controller
commands in `controller/cmd/` gated on a valid config); this one settles what
the states mean, how each is entered, and how each is left.

## Decisions

### Config and mapping

* **No config boots to IDLE.** `ALARM_CONFIG` stops being a machine alarm.
  Controller commands answer `err unconfigured`; primitives are config-free.
  `status cfg` becomes an ungated controller command (e.g. `cfg`).
* **Checks test the config, not `alarmReason`.** `stop` overwrites
  `ALARM_CONFIG` with `ALARM_ESTOP`, after which today's reason checks
  (`axes_enable`, `setorigin`, `unalarm`) miss.
* **Unmapped is not an alarm.** Never mapped counts as nothing requested, so
  the machine is IDLE; only a failed `axis_map` raises `ALARM_NODE_FAULT`.
  `axis_map - - - -` already clears `ALARM_NODE_FAULT` today (the empty map is
  complete), which is the bench escape until this lands.

### Bus sweep: participants and taint

* **One boot sequence** for cold boot, `reset` and an accepted `CFG_SET`.
  Cold boot already enters through `loop()`'s wipe (`soft_reset_requested`
  starts true), so the sweep lives there. `reset` is a loop restart, not a
  chip reset: USB stays up.
* **Participants:** the nodes that answered the latest sweep. Re-swept on every
  pass through the boot sequence, so `reset` brings a recovered node back.
* **Taint:** one bit per node id, set when the node is engaged into a slot.
  Cleared only by power-on (static init); the wipe leaves it. A tainted node
  that goes mute may still be following its old slot.
* **Sweep scope:** the config's node list plus every tainted node (a node
  dropped from a new config may still hold a slot). No config: no sweep, no
  slate; `axis_map` refuses without a config, so nothing can become tainted.
* **Mute after the sweep:** untainted, may be accepted per the strictness
  level; tainted stepper, hard fault until power cycle.
* **Degraded bus** is its own alarm reason (e.g. `ALARM_BUS_DEGRADED`), not
  `NODE_FAULT`. Its exit is "accept the bus as it is": the mute nodes become
  excluded, primitives addressed to them answer `err excluded`, the controller
  maps what is left, and motion needing an excluded axis is refused by the
  primitives (the controller passes the error up).
* **Strictness** is a machine-config field, so bench work needs no reflash:
  * `strict`: any mute node is a hard fault.
  * `peripherals`: mute peripherals may be accepted; a mute stepper may not.
  * `any`: any mute node may be accepted, taint included. Bench only: with
    separate node supplies the taint bit cannot be trusted.
* **Production** puts the Pico and nodes on one switch, so power-on clears
  both the nodes and the taint together.
* **`CFG_SET` → reset.** After the ACK, the Pico runs the boot sequence. Every
  config push voids the datums (the wipe clears them), which avoids judging
  which config fields invalidate a datum. The host sends nothing after the ACK
  until the boot banner; the wipe flushes serial input.

### Slots are freed only by confirmation

* **`axisMapApply` stops treating a silent node as free.** A slot whose
  previous node did not answer the disengage stays unbound and is not given to
  another node. Any reply counts, a NACK included; only a timeout is
  unconfirmed.
* Same rule for the probe restore and the boot map.

### Estop

* **The estop sweep disengages as well as disables.** Today it only disables
  (`core1/core1.cpp:40-60`, `busDisableAll` in `core1/bus/packet.cpp`), so
  every binding, a probe binding included, survives an estop. The sweep already
  walks every bus id and keeps what each node confirmed in `nodeEnabled`
  (Core 1 is its only writer; the replies are otherwise discarded). The
  disengage confirmations go in a second Core-1-written mask the same way.
  Core 0 clears its slot table on the estop edge in `reconcileValidity`
  (`core0/ops/position.cpp`), beside the datum invalidation that already fires
  there.
* The soft-reset park runs the same `busDisableAll`, so disengaging there too
  gives `reset` the same slot release for every node that answers.
* A disable the sweep could not confirm is the same fault as an unconfirmed
  disengage (see the open concern in `busDisableAll`): `unstop` requires both.
* **`unstop`** is the primitive exit from `ALARM_ESTOP`. It re-sends the
  disable and disengage to unconfirmed nodes and refuses to leave until all
  confirm. It
  lands in IDLE, unmapped, de-energised, un-homed.
* An estop ends any homing or probe session: `claimed` is released, the
  probe's saved map is discarded.
* Enabling never clears an alarm (neither `axes_enable` nor `enable`).

### Homing session

* **The first leg from IDLE or `ALARM_LIMIT_LATCHED` enters HOMING**, and the
  machine stays there across legs. Legs are refused in every other ALARM and no
  longer clear the alarm reason at arm.
* Inside the session a seek ending on its switch records the latch without
  publishing `ALARM_LIMIT_LATCHED`; the state is settled once, at the exit.
* **Exits:** `setorigin` commits and leaves; `home_end` abandons without a
  datum; estop and `HOMING_FAIL` leave as today. A session entered from
  `LIMIT_LATCHED` whose switch is still held settles back there.
* **Scope is whatever `setorigin` names.** A slot that ran legs but is not
  named ends un-homed; each leg already invalidates its node's origin.
* **Allowed in a session:** queries, `stop`, `home_end`, `setorigin`.
  Refused: `axis_map`, `step`, jobs, probing.
* A leg changes no slot binding, so homing needs none of the probe's
  containment.

### `setorigin`

* **Syntax:** `setorigin <s1> <s2> <s3> <s4>`, each the slot's position in
  steps or `-` to skip. Exactly four tokens, so every old call answers
  `err usage`. Only `-` skips (unlike `axis_map`, `0` is a position here).
  All four `-` is `err usage`.
* A named slot with no node bound is an error before any bus I/O. A node that
  does not answer `DATUM_SET` gives `err node <id> …`; the other slots still
  go through.
* **No longer clears alarms.** Post-estop recovery becomes `unstop` →
  `axes_enable on` → home or `setorigin`.
* Still valid outside a session, as a manual datum. Keeps the estop-wins check
  (reason snapshot on entry).
* Stays slot-framed: the controller commits a map at boot, and matching
  `axis_map`'s form is worth the map dependency.

### Probe containment

* **The restore moves from failure to exit.** `probeFail` no longer restores
  the map; the probe binding stays live in `ALARM_PROBE_FAIL`, which is what
  lets a leg be retried.
* **`claimed` contains it**, not the alarm reason: `resumeOrHold()` refuses
  IDLE while a probe binding is live, and only `probe_end` clears it. With the
  estop disengaging, the reason can no longer be overwritten under a live
  binding.
* **Recovery:** datum kept (`BUDGET`, `POLL`, `CHATTER`, `ALREADY_OPEN`,
  `NOT_CLEARED`): retry the leg or `probe_end`. Datum void (`POS_MISMATCH`,
  `DEADLINE`): `probe_end` only, then re-home Z.
* `probe_end`'s restore already checks the map; the check moves with it.
* **`PROBE_ESTOP` is removed.** `ALARM_ESTOP` already says why; Core 1's leg
  emitter returns a generic "aborted".

### `unalarm`

* A controller command that dispatches to the reason's primitive exit, and
  never writes state itself:

  | Reason | Exit |
  |---|---|
  | `ESTOP` | `unstop` |
  | `NODE_FAULT` | `axis_map` retry |
  | `BUS_DEGRADED` | accept the bus |
  | `LIMIT_LATCHED` | reverse leg |
  | `HOMING_FAIL` | `home_end` |
  | `PROBE_FAIL` | `probe_end` |
  | `SOFT_LIMIT` | goes with the job path |

### Dropped

* Estop while probing becomes `PROBE_FAIL`: replaced by the estop disengaging.
* Reset-reason or scratch registers for "can a mute node hold a slot":
  `reset` keeps RAM, and per-node taint is more precise.
* A delay after the `CFG_SET` ACK: moves the gap, does not close it.
* Node-addressed `setorigin`: slot form kept (see above).

## Wire changes

All touch `web/src/wire/` and the Sim (`web/src/wire/link/backends/sim.ts`).

* Alarm reasons: `ALARM_CONFIG` retired; `ALARM_BUS_DEGRADED` added.
* Commands: `unstop`, `home_end`, `cfg`; `setorigin` syntax.
* Errors: `err excluded`, `err node <id> …` from `setorigin`.
* `CFG_SET`: the host waits for the boot banner after the ACK.
* Config: the strictness field.

## Branches

Proposed order. Each gets a full Plan section when its turn comes.

1. `feature/config-unmapped`: no config boots to IDLE, unmapped is not an
   alarm, config checks read the config. Depends on nothing.
2. `feature/bus-sweep`: boot sequence sweep, participants and taint,
   strictness, degraded bus, `CFG_SET` → reset, confirmed slot release, estop
   disengage, `unstop`. Uses the existing disable and disengage commands, kept
   behind one per-node "make safe" step so the node-side claim (Open
   questions) can replace it without changing states or exits. Depends on 1.
3. `feature/homing-session`: session states and exits, `home_end`, the new
   `setorigin`, legs only from `LIMIT_LATCHED`. Depends on 1.
4. `feature/alarm-exits`: `unalarm` dispatcher, probe restore at exit,
   `claimed` containment, `PROBE_ESTOP` removed. Depends on 2 and 3.
5. After the planner overhaul: motion gating (motion primitives refuse when
   unmapped, motion controller commands map first) and no jobs without homing.

## Branch 1: `feature/config-unmapped`

**Type:** feature. Changes boot state, alarm reasons and replies.

**Purpose:** no config boots to IDLE; never mapped is not an alarm; the
remaining config checks read the config (or the map), not `alarmReason`.
`ALARM_CONFIG` is retired.

**Files** (under `src/rp2350/core0/` unless noted):

1. State writes:
   * `core0.cpp:113-116`: the wipe stops choosing `ALARM_CONFIG`. It keeps
     `STATE_ALARM` + `ALARM_NODE_FAULT` until `controllerApplyDefaultMap`
     settles it; with no config that call's `resumeOrHold()` now lands IDLE.
   * `ops/state.cpp:12`: drop the `!machineCfgValid()` branch.
   * `ops/axis_map.cpp:103-107`: `axisMapComplete()` returns true when no map
     was ever requested (`!haveRequest`) and stops reading the config;
     `axisMapRetry()` (`:110`) follows. `:124`: `axisMapGate` always raises
     `ALARM_NODE_FAULT`. `axisNodeInConfig` stays (it serves `axis_map`'s
     `not_in_config`).
   * `controller/seq/controller.cpp:11-12`, `controller.h:11`: comments.
2. Checks that read `alarmReason == ALARM_CONFIG`:
   * `cmd/axis.cpp:45-51` (`axes_enable`): refuse when no slot is bound,
     `err unmapped`, instead of testing the reason. Keeps the primitive
     config-free; without a config nothing can be bound.
   * `cmd/axis.cpp:271-276` (`setorigin`): drop the `ALARM_CONFIG` clause.
     Its alarm clearing goes in branch 3.
   * `controller/cmd/unalarm.cpp:13`: drop; the dispatch gate covers it.
3. `cfg`: a new command replacing `status cfg` (`cmd/query.cpp:89-104`), same
   reply. Ungated, so it lives in the primitive table (`cmd/table.h`), not the
   controller table, whose one gate is the config. `status cfg` is removed;
   nothing in `web/` sends it.
4. `ipc/shared_state.h:142`: `ALARM_CONFIG = 2` retired; the value stays
   reserved.
5. Web (wire change):
   * `web/src/wire/format/status.ts:53`: `CONFIG` removed, value 2 reserved.
   * `web/src/wire/link/backends/sim.ts` (`:61`, `:83-88`, `:337`,
     `:547-548`, `:660`, `:721`, `:754`): unconfigured boot is IDLE and
     unmapped; `axes_enable` answers `err unmapped` with nothing bound.
   * `web/src/wire/link/commands.ts:450`: doc comment.
   * Tests: `web/test/wire/format/status.test.ts:52`,
     `web/test/wire/link/backends/sim.test.ts:167,390,404`,
     `web/test/wire/link/commands.test.ts:253,270`.
6. Docs describing the boot gate: `docs/engage_and_axis_map.md` (§6.1),
   `docs/config_storage.md`, `docs/tool_probe.md`. The historical plans
   (`PLAN_*`, `state_redesign.md`, the premortem, `node_session_and_datum.md`)
   stay as written.

**Out of scope:** the bus sweep, `CFG_SET` → reset, and motion gating on an
unmapped machine (branch 5; until then motion from IDLE-unmapped streams to
no slot, as it does today after `axis_map - - - -`).

**Checks:** `pio run -e pico`, `pio test -e native`, `pnpm typecheck` and
`pnpm test`.

**Overlap:** none (`irq-bench` has no branch type).

**Depends on:** nothing.

**Status:** ready to merge. Not yet run on hardware. `pio run -e pico` passes;
`pnpm typecheck` and `pnpm test` pass.

**Outcome:**

* `axes_enable` with nothing bound answers `err unbound`, not `err unmapped`:
  the string `setorigin` and `setprobe` already use for the same condition.
* New `axisMapForget()` (`ops/axis_map`): the wipe drops the requested map
  with the slot table, so a map requested before `reset` cannot outlive it.
  "Never mapped is complete" is keyed on it.
* `status cfg` also had a caller in `web/demo/barebones.js`; it now sends
  `cfg`. A worktree needs `pnpm build` and a `node_modules/urumi-host` link to
  `web/` before `pnpm demo` resolves the package.
* `commands.ts:450` needed no change: `axis_map` still answers
  `err unconfigured`.
* The Sim has no config, so "unconfigured" there means `axisMap: undefined`.
  Its `setorigin` answers `ok` with nothing bound where the firmware answers
  `err unbound` (predates this branch).
* The staging history in `engage_and_axis_map.md` (lines 30, 431, 437) still
  names `ALARM_CONFIG`; left as a record.
* For branch 5: an unconfigured machine is now IDLE, so motion ingest accepts
  a job that streams to no slot. `probe_map` (`err no_z`) and `setprobe`
  (`err unbound`) already refuse. Homing legs ran under `ALARM_CONFIG` before
  and still run; branch 3 restricts where legs start.

## Open questions

* **Node side** (its own session): docs/node_session_and_datum.md §3 and §7
  already design most of it. `SET_SESSION` claims a node with a Pico-chosen
  token and self-safes it (disengage, de-energise, laser off): that is the one
  command every node supports, and ping → claim → engage is the boot sweep.
  A token of 0 means "never configured by this Pico", so taint shrinks to the
  nodes that do not answer the claim. Also there: detecting a node that reboots
  mid-job. Still open beyond it: a node unbinding itself after bus silence,
  which closes the last hole (a mute node holding a slot the Pico has
  forgotten). That doc's §2 (node-frame datum) is built, and its §6 and "no NAK"
  prerequisite are superseded.
* **`alarmReason` has two writers.** Core 1 writes `ALARM_ESTOP`
  (`core1/core1.cpp`) and `ALARM_SOFT_LIMIT` (`core1/emit/microsegment.cpp`);
  Core 0 writes the rest (`ops/`, and the wipe in `core0.cpp`). Safe only
  because the writes never overlap in time. "Only ops write state" covers Core
  0's side; worth a header note (docs/node_state_ingest.md §7).
* Keying datum invalidation on the alarm reason: `originKillGen`
  (docs/node_state_ingest.md §7) becomes worth building when soft limits give
  Core 1 a second path that ends motion abruptly. Not needed here: the estop
  keeps `ALARM_ESTOP`.
* Whether a boot-time broadcast disengage is worth adding before the node-side
  work.
* The homing recipe on the Pico: the controller sequence that holds the
  session, and the handler bodies (`setorigin`'s datum loop, the enables) that
  move into ops for it. The probe session may later follow the same shape.
* `unalarm` behaviour per reason beyond the table above.

## Follow-ups

* Path references outside `src/` from core0-layers: `docs/` (`homing.md`,
  `tool_probe.md`, `node_state_ingest.md`,
  `node_frame_ownership_migration.md`, `PLAN_rp2350_refactor.md`),
  `include/common.h`, web comments (`sim.ts`, `status.ts`, `blob.ts`,
  `controller.test.ts`).
* The Sim still models the old boot gate and has no `CFG_SET`/`CFG_GET`
  (docs/plans/pico-config.md).
* Hardware check of core0-layers: boot, `unalarm`, `axis_map`.
* Stale comment in `core1/core1.cpp`'s estop path: it says Core 0 clears
  `axes_enabled` on ALARM + `ALARM_ESTOP`; `reconcileValidity` now recomputes
  it from `nodeEnabled` every pass.
