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

### Bus sweep: mute, taint, exclusion

* **One boot sequence** for cold boot, `reset` and an accepted `CFG_SET`:
  `loop()`'s wipe (`core0.cpp`, "A: THE SOFT RESET SEQUENCE"; cold boot enters
  it too, `soft_reset_requested` starts true), then the sweep, then the
  default map. `reset` is a loop restart, not a chip reset: USB stays up.
* **Make safe** is one per-node step: disable, then disengage. Any reply
  confirms, a NACK included (only stepper and vacuum nodes have a disengage).
  The disable clears the node's datum witness (`src/node/dispatch.cpp:89`),
  so every node that answers comes out un-homed.
* **The sweep** is make-safe to every bus id 1..`BUS_ADDR_MAX`, then `mute`
  rebuilt from the answers and `excluded` cleared. It runs only in the boot
  sequence, replacing the reset park's own sweep. There is no sweep
  primitive: with slot ids surviving the wipe, `reset` does everything one
  would, and re-applies the default map as well.
* **Node masks** (bit per bus id), both surviving the wipe:
  * `nodeEnabled` (Core 1): confirmed energised.
  * `nodeReleased` (Core 1): confirmed disengaged since the last engage.
  Power-on starts them clean (nothing enabled, everything released), which is
  true only when the nodes power up with the Pico.
* **Taint** is derived, not stored: `nodeEnabled | ~nodeReleased`. After a
  sweep a set bit means a make-safe to that node went unconfirmed, so it may
  still hold torque or follow its slot.
* **Mute:** no answer to the sweep, and either named by the config or
  tainted. An id nobody expects is an empty address.
* **Degraded bus** is its own alarm, `ALARM_BUS_DEGRADED`, held while any mute
  node is not excluded; the default map is not applied. Exits: `bus_exclude`,
  `reset` (a recovered node answers), a `CFG_SET` without the node, power
  cycle.
* **`bus_exclude <id> …`** is a config-free primitive for running on a
  degraded bus. It takes mute ids only (`err not_mute` otherwise). Commands to
  an excluded node answer `err excluded`, so `axis_map` cannot bind it. When
  no unexcluded mute node is left the machine settles IDLE, unmapped.
  Exclusion lasts until the next sweep, so it is decided again after every
  `reset`. `unalarm` (branch 4) picks the ids from the strictness level.
* **Strictness** is a machine-config field, so bench work needs no reflash:
  * `strict`: no mute node may be excluded.
  * `peripherals`: mute peripherals may be excluded; a mute stepper may not.
  * `any`: any mute node may be excluded, taint included. Bench only: with
    separate node supplies the power-on masks cannot be trusted.
  With the fence (below) strictness is policy only: whether to run without a
  node, not whether that is safe.
* **Production** puts the Pico and nodes on one switch, so power-on clears
  the nodes, the masks and the slot ids together.
* **`CFG_SET` → reset.** After the ACK, the Pico runs the boot sequence. Every
  config push voids the datums (the wipe clears them), which avoids judging
  which config fields invalidate a datum. The host sends nothing after the ACK
  until the boot banner; the wipe flushes serial input.

### Slots are freed only by confirmation: the fence

* **A slot keeps its node id until that node confirms the disengage.** Any
  reply counts, a NACK included; only a timeout is unconfirmed. This holds in
  `axisMapApply`'s park, the probe restore, the estop edge and the sweep.
* **A slot held by an unconfirmed node is fenced:**
  * its position and homed bit are cleared and the node's origin invalidated
    at once (`originInvalidate`), since nothing is known about it;
  * no other node is engaged into it; the request-vs-bound comparison
    (`axisMapComplete`) reads the map as incomplete (`NODE_FAULT`);
  * ingest refuses a packet with steps for it (`NACK_BAD_STATE`): a node that
    hears but cannot answer would otherwise follow them. Motion gating in
    general is branch 5.
* **Once its node is excluded**, a fenced slot satisfies a request of `-` for
  that slot, so the rest of the map can complete. It stays fenced.
* **Slot ids survive the wipe.** Like the node masks they record what the
  nodes may still be doing, so only power-on clears them. The wipe clears
  positions, homed bits and the requested map (nothing uses a stale request:
  the boot sequence applies the default map, or none when degraded); the
  sweep then frees every slot whose node confirms. A node that died for good
  keeps its slot fenced until a power cycle; `reset` and `CFG_SET` do not
  clear it.
* **A head switch keeps its datums.** `axis_map` parks with a disengage only
  (`ops/axis_map.cpp:38`), never a disable, so parked and re-engaged nodes
  keep their witness (`parkRecord`/`parkMoved`). Only make-safe (estop, the
  boot sweep, `unstop`) costs the homing. A park that is not confirmed fences
  the slot and ends the switch in `NODE_FAULT`.

### Walkthrough

X = node 1, Y = 2, Z = 3, A = 4 in slots 0..3; vacuum = 5. Homed, IDLE.

```
node 3's cable works loose            nothing polls; still IDLE
axis_map 1 2 3 4 (head change)        1,2,4 park and re-engage, keep datums;
                                      3 silent: slot 2 fenced (id 3, un-homed)
                                      -> ALARM_NODE_FAULT
reset                                 wipe: positions, homed, request cleared;
                                      slot 2 still 3. Sweep: 1,2,4,5 answer,
                                      slots 0,1,3 freed; 3 mute, tainted
                                      -> ALARM_BUS_DEGRADED, no map
  cable fixed, reset                  3 answers, slot 2 freed, default map
                                      -> IDLE mapped; home
  or bus_exclude 3                    -> IDLE unmapped; slot 2 still fenced
     axis_map 1 2 - 4                 slot 2 satisfies `-` -> IDLE
                                      Z steps NACKed; cmds to 3: err excluded
                                      home X, Y, A; 2D work
  later reset                         exclusion gone -> ALARM_BUS_DEGRADED
                                      again unless 3 answers
power cycle                           clean: no ids, no taint
```

### Estop

* **The estop sweep disengages as well as disables.** Today it only disables
  (`core1/core1.cpp:40-60`, `busDisableAll` in `core1/bus/packet.cpp`), so
  every binding, a probe binding included, survives an estop. It becomes
  make-safe to every bus id, recording confirmations in `nodeEnabled` and
  `nodeReleased`.
  On the estop edge Core 0 unbinds every slot whose node is released
  (`reconcileValidity`, `core0/ops/position.cpp`), beside the datum
  invalidation that already fires there.
* A disable the sweep could not confirm is the same fault as an unconfirmed
  disengage (see the open concern in `busDisableAll`): `unstop` requires both.
* The estop's make-safe parks the bus but is not a sweep: `mute` and
  `excluded` are left as they were. `reset` refreshes them.
* **`unstop`** is the primitive exit from `ALARM_ESTOP`. It re-sends
  make-safe to every tainted node that is not excluded and refuses to leave
  until all confirm. It frees every confirmed slot (an excluded node's slot
  stays fenced) and forgets the requested map, then
  settles: `ALARM_BUS_DEGRADED` if unexcluded mute nodes remain, else IDLE,
  unmapped, de-energised, un-homed.
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
  | `BUS_DEGRADED` | `bus_exclude` the mute nodes the strictness allows |
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
* Commands: `unstop`, `bus_exclude`, `home_end`, `cfg`; `setorigin` syntax.
* Errors: `err excluded`, `err node <id> …` from `setorigin`.
* `CFG_SET`: the host waits for the boot banner after the ACK.
* Config: the strictness field.

## Branches

Proposed order. Each gets a full Plan section when its turn comes.

1. `feature/config-unmapped`: no config boots to IDLE, unmapped is not an
   alarm, config checks read the config. Depends on nothing.
2. `feature/bus-sweep`: boot sequence sweep, mute and taint,
   degraded bus and `bus_exclude`, the fence, `CFG_SET` → reset, confirmed slot release, estop
   disengage, `unstop`. Uses the existing disable and disengage commands, kept
   behind one per-node "make safe" step so the node-side claim (Open
   questions) can replace it without changing states or exits. Depends on 1.
3. `feature/homing-session`: session states and exits, `home_end`, the new
   `setorigin`, legs only from `LIMIT_LATCHED`. Depends on 1.
4. `feature/alarm-exits`: `unalarm` dispatcher, the strictness config
   field that picks `bus_exclude`'s ids, probe restore at exit,
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

**Status:** merged (993972a..899e50b). Checked on hardware: unconfigured
boot, `cfg`, `axes_enable` unbound, default map, `unalarm` retry. Unblocks
branches 2 and 3. `pio run -e pico` passes;
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

## Branch 2: `feature/bus-sweep`

**Type:** feature. New alarm reason, commands, replies and boot behaviour.

**Purpose:** the Decisions under "Bus sweep", "Slots are freed only by
confirmation: the fence" and "Estop", except strictness and session endings
(branch 4).

**Settled in planning:**

* Make safe is `busMakeSafe(node)` in `core1/bus/packet.cpp`; the node-side
  claim later replaces its body only.
* `nodeReleased` is cleared when an engage is sent (a lost ack must not leave
  it set) and set on a confirmed disengage. Core 1 sends both, so it stays the
  sole writer.
* `mute` and `excluded` are Core 0 masks in a new `ops/bus.*`, beside
  `busSweep()`, which the boot sequence calls. Participants (config nodes not mute) are derived, for `status` only.
* The sweep runs from Core 0 as RPCs, after the wipe releases Core 1. Worst
  case 8 × `RESPONSE_TIMEOUT_MS` (20 ms) = 160 ms. With no config it still
  runs; only tainted nodes can then be mute.
* Exclusion is checked once, in Core 0's RPC call path: a node-addressed call
  to an excluded node returns a new `RPC_EXCLUDED` (`err excluded`).
  Make-safe is exempt.
* `unstop` waits only on nodes that are not excluded.
* A tainted mute stepper may be excluded by the primitive; its fenced slot
  keeps it safe.
* Until branch 4, `unalarm` in `ALARM_ESTOP` answers `err estop` (its map
  retry would leave the estop unconfirmed).

**Files** (under `src/rp2350/` unless noted):

1. Make safe and the masks:
   * `core1/bus/packet.cpp:65-88`: `busDisableAll` becomes `busMakeSafeAll`
     over `busMakeSafe`; its CONCERN comment is answered by `unstop`.
   * `core1/core1.cpp:37-67` (estop): make-safe; fix the stale
     `axes_enabled` comment at `:52-54`. `:148` (reset park): its sweep moves
     to the boot sequence.
   * `core1/rpc_server.cpp`: engage sent clears `nodeReleased`, confirmed
     disengage sets it.
   * `ipc/shared_state.h:254-283`: `nodeReleased`; `ALARM_BUS_DEGRADED = 8`.
2. Confirmed release:
   * `core0/ops/axis_map.cpp:34-45`: a park with no reply fences the slot
     (keeps the id, voids position, homed and origin); `:52-74`: no engage
     into a fenced slot; `:76-79`: only confirmed slots are freed;
     `axisMapComplete` (`:107`): a fenced slot of an excluded node satisfies
     `-`.
   * `core0/data_plane.cpp:118-141`: ingest refuses steps for a fenced slot.
   * `core0/ops/position.cpp:234` (`reconcileValidity`): on the estop edge,
     unbind the slots whose node is released.
   * `core0/core0.cpp:113` + `core0/ops/position.cpp`: the wipe replaces
     `axisMapReset()` with a clear that keeps slot ids (positions and homed
     bits go); the sweep frees confirmed slots.
   * `core0/ops/probe.cpp`: the restore goes through `axisMapApply` and
     inherits the rule; checked, not changed.
3. Sweep, degraded, `bus_exclude`:
   * New `core0/ops/bus.{h,cpp}`: `busSweep()`, `mute`, `excluded`, taint.
   * `core0/core0.cpp:147-149`: `busSweep()`, then the default map unless
     degraded.
   * `core0/ops/state.cpp`: `resumeOrHold` settles `ALARM_BUS_DEGRADED` first.
   * `core0/cmd/` + `cmd/table.h`: `bus_exclude`.
   * `ipc/core1_rpc.{h,cpp}`: `RPC_EXCLUDED`, the check, its text.
   * `core0/cmd/query.cpp`: `status` reports mute, excluded and tainted (text
     plane only; STATUS_RSP layout unchanged).
4. `unstop`: `core0/cmd/lifecycle.cpp`, `core0/cmd/table.h`;
   `core0/controller/cmd/unalarm.cpp` (`err estop`).
5. `CFG_SET` → reset: `core0/data_plane.cpp:228-240` sends the ACK, then
   raises `soft_reset_requested` instead of applying the map. Web
   `src/wire/link/link.ts:205-226`: after `CFG_ACK`, wait for the boot banner
   (with a timeout) before resolving.
6. Web wire and Sim: `src/wire/format/status.ts` (`BUS_DEGRADED`),
   `src/wire/link/commands.ts` (`unstop`, `bus_exclude`), `sim.ts` (`stop`
   at `:617`, `unstop`, `bus_exclude` answering `err not_mute` since the Sim
   has no mute nodes). Tests beside each.
7. Docs: `docs/engage_and_axis_map.md` (release rule, estop),
   `docs/wire_protocol.md` (commands, errors, `CFG_SET` banner),
   `docs/config_storage.md` (reset after commit).

**Out of scope:** strictness, the `unalarm` dispatcher, session endings on
estop and `claimed` (branch 4); `setorigin` (branch 3; it still clears
`ALARM_ESTOP` until then); the node-side claim.

**Commit units (proposed):** 1+2 release rule and estop make-safe; 4
`unstop`; 3 sweep, degraded, `bus_exclude`; 5 `CFG_SET` reset. Web and docs
travel with the unit they describe.

**Checks:** `pio run -e pico`, `pio test -e native`, `pnpm typecheck`,
`pnpm test`. Human scope: estop with a node unplugged, `reset` with a mute
tainted node, the walkthrough's recovery from `NODE_FAULT`, a head switch
keeping its datums, `CFG_SET` then banner, sweep timing on the bus.

**Overlap:** none (`irq-bench` has no branch type). Touches
`core0/cmd/table.h` and `web/src/wire/`.

**Depends on:** branch 1 (merged).

**Status:** planned.

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
