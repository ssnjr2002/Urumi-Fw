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

### Bus sweep: mute, touched, exclusion

* **One boot sequence** for cold boot and `reset`: `loop()`'s wipe
  (`core0.cpp`, "A: THE SOFT RESET SEQUENCE"; cold boot enters it too,
  `soft_reset_requested` starts true), then the sweep, then the default map,
  then a `ready` line after the banner. `ready` means the sequence is done; the
  banner is informational. `reset` is a loop restart, not a chip reset: USB
  stays up. An accepted `CFG_SET` reaches it through the soft reset.
* **Make safe** is one `CMD_MAKE_SAFE` (branch 1b) to one node. It is
  **confirmed** when the status reply shows no slot (or slot `0xFF`) and
  `NODE_FLAG_ENABLED` clear; a timeout, or a reply showing anything else, is
  unconfirmed. It clears the node's datum witness, so every node that confirms
  comes out un-homed.
* **The sweep** is make-safe to every bus id 1..`BUS_ADDR_MAX`, then `mute`
  rebuilt from the answers and `excluded` cleared. It runs only in the boot
  sequence, replacing the reset park's own sweep. There is no sweep
  primitive: `reset` does everything one would, and re-applies the default
  map as well.
* **`nodeEnabled`** (Core 1, bit per bus id, survives the wipe): confirmed
  energised. Power-on starts it clear, which is true only when the nodes power
  up with the Pico.
* **Touched** is derived, not stored: since power-on or its last confirmed
  make-safe, the Pico may have energised the node or bound it to a slot. The
  node is in `nodeEnabled`, or holds a
  slot (bound or fenced, see below). After a sweep a touched node is one whose
  make-safe went unconfirmed, so it may still hold torque or follow its slot.
* **Mute:** no answer to the sweep, and either named by the config or
  touched. An id nobody expects is an empty address.
* **The silence timeout (1b, opt-in since 1c) does not change what counts as touched.** When
  built in, a node cut off from the Pico is likely safe already, but a node
  that hears the bus and cannot answer looks the same to the Pico, and keeps
  being fed by the keepalive.
* **Degraded bus** is its own alarm, `ALARM_BUS_DEGRADED`, held while any mute
  node is not excluded; the default map is not applied. Exits: `bus_exclude`,
  `reset` (a recovered node answers), a `CFG_SET` without the node, power
  cycle. `slot_map`, `axes_map` and `probe` are refused while degraded
  (`err degraded`): the mute node is decided first, and nothing could run on
  the map anyway.
* **`bus_exclude <id> …`** is a config-free primitive for running on a
  degraded bus. It takes mute ids only (`err not_mute` otherwise). Commands to
  an excluded node answer `err excluded`; make-safe is exempt. When no
  unexcluded mute node is left the machine settles IDLE, unmapped. Exclusion
  lasts until the next sweep. It is not gated by state (in `NODE_FAULT` it
  may exclude a node left mute by the last sweep). It is for leaving `BUS_DEGRADED` and keeping
  commands off a node we cannot hear; a fenced slot needs no exclusion to
  satisfy `-`. `unalarm` (branch 4) picks the ids from the strictness level.
* **Strictness** is a machine-config field, so bench work needs no reflash:
  * `strict`: no mute node may be excluded.
  * `peripherals`: mute peripherals may be excluded; a mute stepper may not.
  * `any`: any mute node may be excluded, touched ones included. Bench only: with
    separate node supplies the power-on state cannot be trusted.
  With the fence (below) strictness is policy only: whether to run without a
  node, not whether that is safe.
* **Production** puts the Pico and nodes on one switch, so power-on clears
  the nodes, `nodeEnabled` and the slot table together.
* **`CFG_SET` → reset.** After the ACK, the Pico runs the boot sequence. Every
  config push voids the datums (the sweep clears them), which avoids judging
  which config fields invalidate a datum. With the silence timeout built in,
  it also covers the nodes that made themselves safe while Core 1 was parked
  for the flash write. The host sends
  nothing after the ACK until `ready`; the wipe flushes serial input.

### Slots are freed only by confirmation: the fence

* **Each slot is `{node id, fenced}`.** The **binding** says who holds the
  slot; the **slot request** (`slot_map`'s argument, or the one `axes_map`
  derives) says what is wanted. A fenced slot always keeps its node id:
  nothing writes `{-, fenced}`. The rules below live in the slot layer
  (`ops/slot_map.cpp`); `axes_map` and the probe inherit them by applying
  through it.
* **A slot is fenced** when a node holding it, or being engaged into it, does
  not confirm: `slot_map`'s park and failed engage (so `axes_map` and the probe
  bind and restore too), `makesafe` with no reply, and, on the estop edge,
  every bound slot.
* **A fenced slot:**
  * has its position and homed bit cleared and its node's origin invalidated
    at once (`originInvalidate`), since nothing is known about it;
  * takes no engage;
  * is refused by ingest for any packet with steps for it
    (`NACK_BAD_STATE`): a node that hears but cannot answer would otherwise
    follow them. Motion gating in general is branch 5;
  * satisfies a request of `-`, so the rest of the map can complete.
* **A fence clears** only on a confirmed make-safe from its node (the sweep,
  `makesafe`, `unstop`, or `slot_map` retrying it, below), or a power cycle.
  Never on time or silence: the silence timer is fed by any byte that passes
  the FERR check, while a park needs a whole frame with a good CRC, so a node
  can keep missing its park and still be fed.
* **A map into a fenced slot** (`slot_map` or `axes_map`, for its own node or
  another) first sends make-safe to the fenced node. Confirmed: the fence
  clears and the engage goes ahead. Unconfirmed: the map fails,
  `ALARM_NODE_FAULT`, with `err fenced <s0> <s1> <s2> <s3>` naming the fenced
  node of each slot the request collides with (`-` elsewhere), e.g.
  `err fenced - - 3 -`. For `axes_map`, that make-safe reply is also the
  node's type check (no separate `CMD_NODE_STATUS`); every unfenced node keeps
  the non-destructive `CMD_NODE_STATUS` check, since make-safe costs its datum.
* **A fenced slot's axis is unbound** (`axisNode(k)` is `-`), so its views
  drop and ingest's unbound-axis refusal (1d) covers it.
* **Readback:** `slot_map` with no argument prints the binding with fenced
  slots marked, `slot_map 1 2 !3 4`; `axes_map` prints the request with
  pending axes marked, `axes_map 1 2 ?3 4` (1d).
* **`makesafe <id>`**: confirmed, the node's slot is unbound and the node is
  dropped from the slot request and, for an axis node, from the axes request
  (as `-`, not pending), so no alarm follows; unconfirmed, the slot is fenced
  (`NODE_FAULT`).
* **The slot table survives the wipe**, bindings and fences both. It records
  what the nodes may still be doing, so only power-on clears it. The wipe
  clears positions, homed bits and the requested map; the sweep then frees
  every slot whose node confirms. A node that died for good keeps its slot
  fenced until a power cycle; `reset` and `CFG_SET` do not clear it.
* **`nodeReleased` is not needed:** a node is released when it holds no slot,
  bound or fenced.
* **A pending axis is handled per slot.** While axis k is pending (1d:
  `axes_map` could not confirm its type), slot k keeps its node in the slot
  request but is parked, not engaged; the other slots bind as requested.
  Replaces 1d's "pending parks every holder": healthy axes are not parked and
  re-engaged, so only the slots that change risk a fence. The request is
  unmet, so `NODE_FAULT` holds until k clears.
* **A head switch keeps its datums.** `slot_map` parks with a disengage only
  (`ops/slot_map.cpp:34`, `applySlots`), never a disable, so parked and re-engaged nodes
  keep their witness (`parkRecord`/`parkMoved`). Only make-safe (estop, the
  boot sweep, `unstop`, `makesafe`, a fence retry) costs the homing. A park
  that is not confirmed fences the slot and ends the switch in `NODE_FAULT`.

### Walkthrough

X = node 1, Y = 2, Z = 3, A = 4 in slots 0..3; vacuum = 5. Homed, IDLE.

```
node 3's cable works loose            nothing polls; still IDLE
axes_map 1 2 3 4 (head change)        type check: 3 silent, axis Z pending,
                                      slot 2 skipped; 1,2,4 park and
                                      re-engage, keep datums; 3's park
                                      unconfirmed: slot 2 fenced
                                      -> ALARM_NODE_FAULT, err node 3 timeout
                                      slot_map 1 2 !3 4, axes_map 1 2 ?3 4
  axes_map 1 2 - 4                    fenced slot 2 satisfies - -> IDLE
                                      Z steps NACKed; X, Y, A keep datums
  cable fixed, axes_map 1 2 3 4       3 fenced: make-safe instead of the type
                                      check; confirmed, stepper, fence cleared,
                                      3 engaged -> IDLE; home Z
  (still broken: axes_map 1 2 3 4     make-safe unconfirmed -> NODE_FAULT,
                                      err fenced - - 3 -)
  or reset                            wipe; sweep: 1,2,4,5 answer, slots
                                      0,1,3 freed; 3 mute (holds slot 2)
                                      -> ALARM_BUS_DEGRADED, no map
     bus_exclude 3                    -> IDLE unmapped; slot 2 still !3
     axes_map 1 2 - 4                 -> IDLE; cmds to 3: err excluded
     later reset                      exclusion gone -> ALARM_BUS_DEGRADED
                                      again unless 3 answers
power cycle                           clean: no ids, no fences, nothing touched
```

### Estop

* **The estop sweep makes safe as well as disables.** Today it only disables
  (`core1/core1.cpp:44-80`, `busDisableAll` in `core1/bus/packet.cpp`), so
  every binding, a probe binding included, survives an estop. It keeps the
  broadcast `CMD_DISABLE` (the stop every node starts at once), then sends
  make-safe to every bus id, before the ALARM transition. The replies update
  `nodeEnabled` only.
* **On the estop edge Core 0 fences every bound slot**, beside the datum
  invalidation that already fires there (`reconcileValidity`,
  `core0/ops/position.cpp`). Core 1's replies are not handed over: `unstop`
  confirms from Core 0, so fences have one clearing path.
* A disable the sweep could not confirm is the same fault as an unconfirmed
  release (see the open concern in `busDisableAll`): `unstop` requires both.
* The estop's make-safe parks the bus but is not a sweep: `mute` and
  `excluded` are left as they were. `reset` refreshes them.
* **`unstop`** is the primitive exit from `ALARM_ESTOP`. It sends make-safe to
  every touched node that is not excluded and refuses to leave until all
  confirm; each confirmation frees that node's slot and clears its fence (an
  excluded node's slot stays fenced). It forgets the requested map, then
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
  `reset` keeps RAM, and per-node touched state is more precise.
* A delay after the `CFG_SET` ACK: moves the gap, does not close it.
* Node-addressed `setorigin`: slot form kept (see above).
* `nodeReleased`: derived from the slot table (bound or fenced).
* A cold-boot wait of `BUS_SILENCE_MS` before Core 1's first byte: a node that
  powers up with the Pico starts its timer only after its address blink, so
  the wait proves nothing.
* Clearing a fence on time or bus silence: a node that missed its park is fed
  by the keepalive. Only a reply clears it.
* Exclusion as a condition for a fenced slot to satisfy `-`.

## Wire changes

The host wire. The web host's side (`web/src/wire/`, the Sim) is deferred from
branch 1d on (see 1d, Web deferred).

* Alarm reasons: `ALARM_CONFIG` retired; `ALARM_BUS_DEGRADED` added.
* Commands: `unstop`, `bus_exclude`, `home_end`, `cfg`; `setorigin` syntax.
* Errors: `err excluded`, `err degraded`, `err fenced <s0> <s1> <s2> <s3>`, `err node <id> …`
  from `setorigin`.
* `slot_map` readback marks fenced slots (`!3`); `axes_map` and `slot_map`
  readbacks (branch 1d).
* Boot: a `ready` line after the banner. `CFG_SET`: the host waits for it
  after the ACK.
* Config: the strictness field.
* Node bus (`include/common.h`, branches 1a and 1b): `CMD_MAKE_SAFE`,
  `CMD_BUS_STATS`, a zero-stream-byte keepalive. Not on the host wire; the host sees
  the counters through the `busstat` text command only.

## Branches

Proposed order. Each gets a full Plan section when its turn comes.

1. `feature/config-unmapped`: no config boots to IDLE, unmapped is not an
   alarm, config checks read the config. Depends on nothing.
1a. `feature/bus-noise`: nodes drop framing-error bytes and count bus
   errors; `CMD_BUS_STATS`, `busstat`. Depends on nothing.
1b. `feature/node-make-safe`: `CMD_MAKE_SAFE` answered with a status payload;
   a node makes itself safe after bus silence; Core 1 keepalive. Depends on
   1a.
1c. `feature/silence-opt-in`: the node silence timeout becomes opt-in
   (`NODE_HAS_SILENCE_TIMEOUT`), off by default. Depends on 1b.
1d. `feature/slot-map`: slots and axes split. `slot_map` binds any node to
   the four stream slots; `axes_map` (renamed from `axis_map`) is the stored
   axis request; an axis is bound when the two agree. Fixes the probe vacuum
   never being released. Depends on nothing.
1e. `fix/rpc-stale-reply`: the Core 0 → Core 1 call split into a
   non-blocking start and finish; `rpcCall` becomes a loop over them. Fixes a
   call collecting another's reply (the probe leg's, or a late one). Gives
   branch 2's exclusion check one home. Depends on nothing.
2. `feature/bus-sweep`: boot sequence sweep, mute and touched nodes, degraded bus
   and `bus_exclude`, the fence, `CFG_SET` → reset and `ready`, confirmed slot
   release, estop make-safe, `unstop`. Make safe is one `CMD_MAKE_SAFE` per
   node. Its slot rules live in `slot_map`. Depends on 1, 1b, 1d and 1e.
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

## Branch 1a: `feature/bus-noise`

**Type:** feature. New node behaviour, one new node command, one Pico
primitive.

**Purpose:** a node never acts on a byte the UART flagged as corrupt, and
keeps count of bus errors for diagnostics. Branch 1b's silence timeout depends
on it: without the filter, noise on an undriven bus (no fail-safe biasing is
known) would keep the timeout fed.

**Settled in planning:**

* One check at the top of both RX ISRs (`src/node/rs485/isr_generic.cpp:20`,
  `src/node/types/stepper/stepper.cpp:731`): a byte with `USART_FERR_bm` set
  in `RXDATAH` (already read there) is dropped, resets framing
  (`frame_stream_reset`) and counts. Before the 9th-bit test, since `DATA8`
  is not trustworthy on a framing error. Stream bytes included: a corrupt
  byte loses a step instead of taking a random one, and a dropped probe
  reply reads as "open" on the Pico (`probe_slot.h`).
* Build flag `NODE_IGNORE_FERR`: framing-error bytes are counted but processed
  as before (bench comparison, or a bus that needs it). Listed in AGENTS.md
  with the other node flags.
* Counters (`uint16_t`, wrapping): FERR, BUFOVF (`RXDATAH` bit 6), and CRC
  failures of frames addressed to this node (`src/node/main.cpp:62`). Never
  cleared; readers take differences, and only power-on resets them. Written
  in the ISR (FERR, BUFOVF), so `loop()` reads them under `ATOMIC_BLOCK`.
* No threshold, latch or flag: rejection is per byte, and the counters are
  diagnostics only. The status payload is unchanged.
* `CMD_BUS_STATS` (0x08), generic, no payload, not broadcastable; the reply
  is the three counters.
* Pico: a new primitive `busstat <node>` prints
  `node <id> ferr <n> ovf <n> crc <n>`. `nodestat` is unchanged (the web
  parses it with an anchored regex, `web/src/wire/link/commands.ts:138`).

**Files:**

1. `include/common.h`: `CMD_BUS_STATS` and its reply layout.
2. `src/node/rs485/isr_generic.cpp`, `src/node/types/stepper/stepper.cpp`
   (RX ISR): the FERR check and counts.
3. `src/node/rs485/rs485.{h,cpp}`: the counters; `src/node/main.cpp:62`: the
   CRC count; `src/node/dispatch.cpp`: the `CMD_BUS_STATS` handler.
4. Pico: `core1/rpc_server.cpp` (`buildPayload` sends none),
   `core0/cmd/query.cpp` + `cmd/table.h`: `busstat`.
5. Docs: `docs/wire_protocol.md` (`busstat`), the node bus doc
   (`CMD_BUS_STATS`, FERR rejection).

**Checks:** `pio run` for every node env, `pio run -e pico`. Human scope:
reflash every node; `busstat` on a healthy bus reads zero; a job, a home and
a probe run unchanged.

**Depends on:** nothing.

**Status:** done.

**Outcome:**

* As planned, plus a Core 0 wrapper `rpcBusStats()` in
  `ipc/core1_rpc.{h,cpp}` (not in the file list) and the `busstat` row in
  `core0/control_plane.cpp`'s table.
* `frame_rx_reject(status)` (`src/node/rs485/frame.h`) is the one check both
  ISRs call; 1b's silence timeout feeds from bytes it lets through.
* Merged with a merge commit at the user's request, though under 10 commits.
* Human scope open: reflash every node; `busstat` zero on a healthy bus; a
  job, a home and a probe unchanged.

## Branch 1b: `feature/node-make-safe`

**Type:** feature. New node command and node behaviour, Pico keepalive.

**Purpose:** one node command every type answers that leaves the node
de-energised and disengaged and says so, and a node that makes itself safe
when the Pico goes quiet. Branch 2's make safe becomes one transaction whose
reply is proof, not an ack to interpret.

**Settled in planning (revised in Read):**

* `CMD_MAKE_SAFE`, generic, in `src/node/dispatch.cpp` beside `CMD_DISABLE`:
  `node_set_enabled(false)`, clear `ENABLED | DATUM`, call a new per-type
  `node_release()` hook, reply with `buildNodeStatus`. Not broadcastable: the
  reply is the point.
* `node_release()`, the type-specific half of make safe:
  * stepper: drops its slot (as `CMD_ENGAGE` with `SLOT_NONE`). The laser
    (`NODE_HAS_LASER`, a 5 mW crosshair) is left alone.
  * vacuum: parks every servo at 0° (closed, as at boot); with
    `NODE_HAS_PROBE_REPLY`, drops `probeSlot`. The pump is already stopped by
    `node_set_enabled(false)`.
  * knife: nothing; `node_set_enabled(false)` already stops both outputs.
* The vacuum status tail gains `[slot]` on `NODE_HAS_PROBE_REPLY` builds only
  (`[servo bits][ssr][slot]`), appended like the stepper's homing span; the
  Pico decodes on minimum length. Released = no slot in the tail, or slot ==
  `0xFF`; enabled = `NODE_FLAG_ENABLED`. No new flag.
* **Silence timeout:** any byte that passes 1a's FERR check (stream or command,
  any address) sets a flag in the ISR; `loop()` turns it into a timestamp. No
  bytes for `BUS_SILENCE_MS` (~1 s) runs the same routine as `CMD_MAKE_SAFE`.
  The node stays up and answers the next frame. Z drops on timeout, as on
  estop. A CRC-gated feed is not possible: foreign frames are dropped at the
  address byte (`rs485/frame.h:32`), and stream bytes carry no CRC.
* **Keepalive:** Core 1 sends one zero stream byte (`writeStream(0)`, the
  byte `busQuiesce` already sends before every transaction) from `processBus`
  when the queue is empty and it has sent nothing for ~`BUS_SILENCE_MS / 3`.
  Every Pico transmission counts as a send. No new opcode: a zero byte steps
  nothing, leaves DIR alone, and gets no probe reply.
* No keepalive during `core1FlashPark`: a `CFG_SET` commit longer than
  `BUS_SILENCE_MS` makes every node safe (IDLE/ALARM only), which costs a
  re-home.
* No hardware WDT.
* The Pico's rules do not change: a mute node may be cut off (and safe) or
  deaf-but-listening (TX broken, still following its slot), and the Pico
  cannot tell which. Branch 2 still fences every mute node; this is defence
  in depth.
* Relation to `SET_SESSION` (Open questions): this is its self-safe half;
  the session token can be added to it later.

**Files:**

1. `include/common.h`: `CMD_MAKE_SAFE`, `BUS_SILENCE_MS`, the vacuum tail.
2. `src/node/dispatch.cpp`, `src/node/node_hooks.h`, each type's
   `node_release()` (`types/stepper`, `types/knife`, `types/vacuum`); the
   vacuum `node_status` slot byte.
3. `src/node/main.cpp`, both RX ISRs (or `rs485/frame.h`): the heard flag and
   the timeout.
4. Pico: `core1/core1.cpp` (`processBus`) keepalive; `core1/bus/` stamps the
   last send time; `core1/rpc_server.cpp` `answersWithStatus` gains
   `CMD_MAKE_SAFE`.
5. Docs: `docs/engage_and_axis_map.md` (make safe, silence), the node bus doc.

**Checks:** `pio run` for every node env, `pio run -e pico`. Human scope:
reflash every node; make-safe to each type; unplug the Pico end of the bus
and see every node go safe; a job and a home run without a timeout.

**Depends on:** 1a.

**Status:** done.

**Outcome:**

* Added a Pico primitive `makesafe <node>` (`core0/cmd/periph.cpp`), gated
  IDLE/PAUSED/ALARM, printing `node <id> en <0|1> datum <0|1> slot <n|->`. It
  leaves the axis map alone; branch 2 decides what a released node means for
  the map.
* `CMD_MAKE_SAFE` is `0x09`; `BUS_KEEPALIVE_MS` = `BUS_SILENCE_MS / 3` (333 ms).
  The node routine is `node_make_safe()` (core-provided, `node_hooks.h`).
* The feed flag is set in `frame_rx_reject` itself, so both RX ISRs share it.
  `NODE_IGNORE_FERR` builds feed on FERR bytes too.
* The Pico side is a flag in `RS485Bus`'s writes, read once per `processBus`
  pass; the keepalive does not need the queue empty, since `processBus` only
  runs between segments.
* `NODE_DEBUG_CONSOLE` builds have no silence timeout (bench use over USART0
  with no Pico on the bus).
* A segment slower than one step per `BUS_KEEPALIVE_MS` would starve the
  timer mid-job; no real feed rate is that slow.
* For branch 2 (out of scope here): soft reset after a `CFG_SET` commit, so a
  commit leaves every node safe every time rather than only when the flash
  write outlasts the timeout.
* Bench: `makesafe 4` dropped node 4's slot; pulling the Pico off the bus
  dropped it too. Still open: vacuum and knife make-safe, a job, a home and a
  probe without a timeout, a pause longer than 1 s.

## Branch 1c: `feature/silence-opt-in`

**Type:** feature. Changes a node default behaviour behind a new build flag.

**Purpose:** 1b's silence timeout is off unless a node env asks for it. On a
flaky bus an outage of `BUS_SILENCE_MS` or more makes nodes safe without the
Pico knowing (nothing polls), and an idle status poll to catch it would add
up to `RESPONSE_TIMEOUT_MS` to every operator command. Deferred (Open
questions); the flag keeps it available.

**Settled in planning:**

* New node flag `NODE_HAS_SILENCE_TIMEOUT`. Without it, `busHeard` and
  `busSilenceCheck` compile out. It is an `#error` with `NODE_DEBUG_CONSOLE`
  (`rs485/rs485.h`): console input does not feed the timer. No env sets it.
* The Pico keepalive stays: one byte per `BUS_KEEPALIVE_MS`, so a node built
  with the flag needs no Pico change.
* `CMD_MAKE_SAFE`, `node_release()` and `makesafe` are unchanged; branch 2
  needs them, not the timeout.

**Files:**

1. `src/node/main.cpp` (`busSilenceCheck`), `src/node/rs485/frame.h`
   (`frame_rx_reject` sets `busHeard`), `src/node/rs485/rs485.{h,cpp}`
   (`busHeard`).
2. `include/common.h`: the `CMD_MAKE_SAFE` / `BUS_SILENCE_MS` comment;
   `src/rp2350/core1/core1.cpp`: the keepalive comment.
3. `AGENTS.md`: the flag in the node build flag list.
4. Docs: `docs/node_type_architecture.md` (the silence paragraph),
   `docs/engage_and_axis_map.md` §4.4.

**Checks:** `pio run` for every node env, `pio run -e pico` (unchanged, but
`common.h` is shared). Human scope: a node without the flag stays enabled
with the Pico unplugged; a node built with it still goes safe.

**Depends on:** 1b.

**Status:** done.

**Outcome:**

* `NODE_HAS_SILENCE_TIMEOUT` with `NODE_DEBUG_CONSOLE` is an `#error`
  (`rs485/rs485.h`) rather than console builds skipping the timeout.
* Also changed: the keepalive comment in `src/rp2350/core1/core1.cpp`.
* Checked with the flag forced on (`PLATFORMIO_BUILD_FLAGS`): `db_node4`,
  `vac_db_node7`, `knife_node8` build; `db_node4_dbg` stops at the `#error`.
* Human scope open: without the flag a node stays enabled with the Pico
  unplugged; with it the node still goes safe.

## Branch 1d: `feature/slot-map`

**Type:** feature. New primitive, a renamed command, new ingest refusal.

**Purpose:** the slot table does two jobs: which node listens on a stream slot
(any type), and which axis that slot is (position, homed, latched, enabled).
`slotBind` writes both, so the probe vacuum is kept out of the table, and
nothing ever releases it: `probeRestore` is `axisMapApply(savedMap)`, whose
park loop reads only the table (`core0/ops/probe.cpp:90-95`, `:202-222`;
`ops/axis_map.cpp:34-45`). After `probe_end`, `probeFail` or a failed
`probe_map` the vacuum still holds slot 3 and answers every A step byte
(`src/node/types/vacuum/probe_slot.h:35-46`), colliding with the stream.
`docs/tool_probe.md:499` says the exit disengages everything. Splitting the
layers fixes this by construction and gives branch 2 one place for its rules.

**Model:**

```
axes_map   the last axis request:    X=1 Y=2 Z=3 A=4   (stored)
slot_map   who holds each slot:      1 2 3 4           (binding, from the bus)
axis k is bound  <=>  slot_map[k] == axes_map[k]
```

A probe is `slot_map - - 3 6`: only Z is bound, slot 3 is lent to the vacuum.
The stream stays `dx dy dz da` into slots 0..3: axis k is slot k.

**Settled in planning:**

* **`slot_map <n0> <n1> <n2> <n3>`**, a config-free primitive (`cmd/`,
  IDLE/PAUSED/ALARM). Binds any node type: parks every node that holds a slot,
  engages the requested ones, stores the slot request. `-`/`0` = empty;
  `err dup` for a node twice. It knows nothing about axes. Refused in RUNNING
  (as `axis_map`) and in `STATE_PROBING` (`err bad_state`), so a probe's
  binding cannot change underneath it.
* **Readbacks:** `slot_map` prints the binding (`slot_map - - 3 6`), `axes_map`
  the request (`axes_map 1 2 3 4`). A slot token is `-`, `n` or `!n` (fenced,
  branch 2; 1d never prints it); an axes token is `-`, `n` or `?n` (pending).
  `slot_map` is a firmware and console primitive; a host needs only
  `axes_map`.
* **`axes_map <x> <y> <z> <a>`** replaces `axis_map` (renamed: matches
  `axes_enable`, `axes_homed`). Keeps today's config checks (`err
  unconfigured`, `not_in_config`): it is the production command, `slot_map`
  the config-free bench one. The axes request is `{node, pending}` per axis;
  pending = not yet confirmed a stepper. `axes_map` stages the request with
  every named axis pending, then sends `CMD_NODE_STATUS` to each pending node:
  * stepper: pending clears;
  * confirmed non-stepper: `err node <id> not_stepper`, the stage is thrown
    away, nothing changes;
  * silent: stays pending, `err node <id> timeout`.
  Otherwise the stage is committed and the slot request set to its ids;
  applied (park, engage) only when nothing is pending, else the binding no
  longer matches and the machine goes `NODE_FAULT`. The boot default map keeps
  a wrong type as pending instead of throwing the stage away, so a wrong config
  boots into `NODE_FAULT`. An axis is bound when its slot holds the requested
  node and it is not pending. `unalarm` re-checks only the pending axes, then
  applies; a pending node that answers as a non-stepper loops there, accepted.
  Readback marks pending as `?n` (`axes_map 1 2 ?5 4`). The
  re-apply (`unalarm`'s retry, the probe exit, the boot default map) is the
  internal `axesMapApply()` with the stored request.
* **Two comparisons:**
  * slot request vs binding: a node that did not do what was asked, so
    `ALARM_NODE_FAULT`. A bus fact.
  * `axes_map` vs binding: which axes are bound. Information, not an alarm.
  The `STATE_PROBING` exemption in `axisMapGate` goes: `NODE_FAULT` never
  reads axes.
* **No request, no axes:** with no `axes_map` since the last wipe (no config,
  or a bench `slot_map` only) no axis is bound, so ingest refuses motion.
  Today no config binds nothing either; `slot_map` on the bench binds nodes
  for `enable`, `nodestat`, `makesafe` and `step`, not for jobs.
* **Axis state only for bound axes:** `machinePos`, `axes_homed`,
  `homingLatched` and `axes_enabled` are derived for axis k only while it is
  bound. An unbound axis reads unbound, not "at 0". The node-frame datum
  (`nodeOrigin`, `nodeHomed`) is unchanged.
* **Ingest refuses a nonzero delta for an unbound axis** (`MSEG_NACK_BAD_STATE`,
  jobs and jogs, `core0/data_plane.cpp:133-171`). Otherwise a job's `da` would
  reach a vacuum in slot 3 as probe queries. Probe legs are Core 1's own
  emitter and do not pass ingest.
* **Probe:** `probe_map <vac>` is `slot_map - - <z> <vac>` plus the session;
  `probe_end` (and `axes_map` during `STATE_PROBING`) re-applies `axes_map`
  plus the teardown. The vacuum is an ordinary slot entry, so the park loop
  releases it. `err vac_mapped` becomes "the vacuum is an axis in
  `axes_map`".
* `unalarm`'s retry re-applies the slot request, not `axes_map`:
  `NODE_FAULT` is about the last `slot_map`, which may be a bench one. When
  the slot request came from `axes_map`, the pending axes are re-checked
  first.
* Callers that mean "the axis" (`axes_enable`, `setorigin`, the Z lookups,
  `status`) read a new `axisNode(k)`, the node only while axis k is bound, so a
  vacuum in slot 3 is never enabled or datumed as an axis. `step` and the hall
  scan look up by node and stay.
* `setorigin` and `step` stay slot-framed (axis k = slot k); branch 3 decides
  `setorigin`'s form.
* `slot_map` is a primitive and `axes_map` stays in the primitive table as
  today (`control_plane.cpp:47`), gated on the config inside; moving it to the
  controller table is not needed for this branch.
* **Web deferred.** The controller is moving onto the Pico, so the web host is
  not ported to this layer yet: it waits until the Pico controller settles, at
  the earliest after branch 3. Until then the web host against 1d firmware
  cannot bind a head or home (it sends `axis_map`, now unknown); the Sim still
  accepts `axis_map`, so `pnpm test` stays green. No `axis_map` alias: its
  readback would still fail `readAxisMap`. Web to port: `src/wire/link/
  commands.ts:433-485` (`axisMap`, `readAxisMap`), `backends/sim.ts`,
  `src/controller/controller.ts:62,368,424`, `src/homing/sequence.ts:227,329`,
  `src/index.ts`, comments in `slots.ts`, `status.ts`, `settled.ts`, demo
  `comms.{html,js}`, and their tests.

**Files** (under `src/rp2350/` unless noted):

1. `core0/ops/axis_map.{h,cpp}` → the slot layer (`slotMapApply`, slot
   request, `slotMapComplete`, gate) and the axis layer (`axesMapApply`, axis
   request, `axisBound(k)`); `core0/ops/position.{h,cpp}`: `slotBind` binds any
   type, axis state derived for bound axes only (`slotAdoptStatus`,
   `reconcileValidity`'s projection at `:234`).
2. `core0/cmd/axis.cpp:130-189` (`cmdAxisMap` → `cmdAxesMap`), a new
   `cmdSlotMap`; `cmd/table.h`, `core0/control_plane.cpp:47`.
3. `core0/ops/probe.{h,cpp}`: `probeBegin`/`probeRestore`/`probeExit` onto
   `slot_map` and `axesMapApply`; `cmd/table.h:65` comment.
4. `core0/data_plane.cpp`: the unbound-axis refusal.
5. Callers: `core0/ops/state.cpp` (`resumeOrHold`),
   `core0/controller/cmd/unalarm.cpp`, `core0/controller/seq/controller.{h,cpp}`
   (default map), `core0/core0.cpp:113-114` (wipe), `ops/homing.h`,
   `ipc/shared_state.h` comments.
6. Docs: `docs/engage_and_axis_map.md` (the two layers), `docs/tool_probe.md`
   (§5, the exit), `docs/wire_protocol.md`, `docs/homing.md`,
   `docs/config_storage.md`. Historical plans stay as written.

**Out of scope:** the fence, touched, sweep and `err fenced` (branch 2, built
on `slot_map`); `setorigin`'s form (branch 3); a stream format not fixed to
four axes (planner overhaul).

**Checks:** `pio run -e pico`. Human scope: `nodestat <vac>` after `probe_end` shows no slot
(before: slot 3); a probe session then an A jog with the vacuum on the bus
runs clean (`busstat` unchanged); `slot_map` binding a vacuum with no config;
a head switch through `axes_map` keeping its datums; a job refused while an
axis is unbound.

**Overlap:** none among open typed branches. Touches `core0/cmd/table.h`.

**Depends on:** nothing (1, 1a, 1b merged).

**Status:** done. Unblocks branch 2.

**Outcome:**

* Modules: `core0/ops/axis_map.*` is gone. `ops/slot_map.{h,cpp}` holds the
  slot request, the apply, `slotMapComplete` and the `NODE_FAULT` gate, and
  knows nothing about axes; `ops/axes_map.{h,cpp}` holds the type checks and
  applies through `slotMapCommit(req, fromAxes, parkOnly, &res)`. The axes
  request (`axesReqSet`, `axesReqAt`, `axesReqPending`, `axisNode`,
  `nodeAxis`) lives in `position.{h,cpp}` beside the views it decides.
  `unalarm` picks the retry (`slotMapFromAxes()`). Branch 2's fence, touched
  and `err fenced` go in `slot_map.cpp`.
* Deviation: while an axis is pending, `axes_map` parks every slot holder and
  binds nothing, rather than leaving the old binding. Otherwise a probe exit
  that hits a timeout would leave the vacuum in slot 3.
* `step` and `hallscan` check the node's own `nodeEnabled` bit instead of
  `axes_enabled`, so a node bound by `slot_map` alone can step.
* `CFG_SET` re-commits the default map through the same routine as boot, so
  it also keeps a wrong type pending (`NODE_FAULT`) instead of refusing.
* `probeBegin` checks types by `CMD_NODE_STATUS` before rebinding, then
  applies `slot_map - - z vac` (`probeBind`); `savedMap` is gone. A `newMap`
  refused for a wrong type on the exit falls back to the stored request.
* Web deferred (see Settled): the web host sends `axis_map`, now unknown, so
  against this firmware it cannot bind a head or home. Its Sim still accepts
  `axis_map`, so `pnpm test` is green and proves nothing about 1d.
* Out of scope: the `axes_map`-as-probe-exit route (`cmd/axis.cpp`,
  `probeExit(newMap)`) is unreachable, as it was for `axis_map` on main:
  `busGateDenies()` refuses `STATE_PROBING` first. `probe_end` is the only exit.
* Checks: `pio run -e pico` clean. Human scope open, as listed in Checks, plus
  `axes_map` with a silent node (`err node <id> timeout`, `NODE_FAULT`, then
  `unalarm` once it answers) and with a config axis id that answers as a
  non-stepper (`not_stepper`, nothing changes; a vacuum id is already
  `not_in_config`).

## Branch 1e: `fix/rpc-stale-reply`

**Type:** fix. The Core 0 → Core 1 call split into start and finish, which
fixes two ways a call collects a reply that is not its own. Retyped from
`refactor/rpc-start-finish` after its Read (below).

**Purpose:** one entry point every Core 0 → Core 1 request passes through,
with a result, so branch 2 checks exclusion once; and every reply matched to
its own request. Today `rpcCall` (`ipc/core1_rpc.cpp:64`) is the only path
that stamps an id, waits and checks the reply's echo; `rpcStepDebug` (`:329`)
and `rpcProbeLegPost` (`:367`) call `rpcPost` (`:31`) directly and get a bool,
and `probeTick` (`core0/ops/probe.cpp:389-399`) polls with `rpcPoll` and checks
the id only.

**The bugs (Read findings):**

1. **The probe leg is posted with id 0.** `rpcProbeLegPost` never sets
   `req.id`, so `rpcPost` treats it as fire-and-forget and takes no in-flight
   claim, while Core 1 still replies (echoing id 0, `core1/rpc_server.cpp:27`)
   and `probeTick` matches 0 against `legId` 0. An `rpcCall` issued while a
   leg runs queues behind it and collects the leg's reply: echo mismatch,
   `RPC_BAD_REPLY`. The path that does this: the leg deadline
   (`probe.cpp:431`) → `probeFail` → `probeRestore` → `axesMapApply`'s status
   and engage calls, while Core 1 may still be finishing the leg.
2. **A late reply poisons the next call.** `rpcCall` drops the in-flight
   claim on timeout, but Core 1's reply can still arrive; the next caller's
   `rpcPoll` takes it and fails the echo check. Same after a leg deadline.

**Shape:**

* `rpcStart(req, &id)`: stamps an id on every reply-bearing request (the
  probe leg included; fix 1), takes the in-flight claim, posts. Only
  `RPC_OP_STEP_DEBUG` stays id 0. Returns an `RpcResult`; busy or a full
  queue is `RPC_TIMEOUT`, as `rpcCall` reports it today.
* `rpcFinish(id, out)`: never blocks. `RPC_PENDING` (new) until its reply is
  collected; a reply with another id is discarded, not failed on (fix 2).
  Then the echo check (cmd, node) and the NAK reason that `rpcCall` does now.
* `rpcCall`: `rpcStart`, then `rpcFinish` in a loop until
  `RPC_CALL_TIMEOUT_MS`. Its signature and every caller unchanged.
* `rpcPost` and `rpcPoll` become file-static in `core1_rpc.cpp`.
* `rpcStepDebug` and `rpcProbeLegPost` return an `RpcResult` through
  `rpcStart`; `probeTick` finishes with `rpcFinish`. Their callers print what
  they print today, except that a call made during a leg now answers busy
  instead of reading the leg's reply.

**Files:** `ipc/core1_rpc.{h,cpp}` (API and its header comments, `:150-160`,
`:260-275`); `core0/ops/probe.cpp:329,389-399`; `core0/cmd/axis.cpp:449,502`
(`rpcStepDebug`'s result); the file-header notes naming `rpcPost/rpcPoll`.

**Out of scope:** the exclusion check itself (branch 2); converting other
blocking callers (`rpcHome` blocks a whole homing leg in `rpcCall`); what a
leg deadline should do about a leg Core 1 is still running.

**Checks:** `pio run -e pico`. No failing test first: there is no test
harness for `ipc/`, and both bugs need Core 1 running. Human scope: a probe
session start to end, and `step`, on the machine; a `nodestat` and an
`axes_map` to show `rpcCall` unchanged; bug 1 on the bench, a leg whose
deadline fires (a short `deadlineUs`) with the restore then binding cleanly
(before: `err`/`NODE_FAULT` from a `bad_reply`).

**Overlap:** none among open typed branches. `feature/bus-sweep`'s worktree
exists with no commits; it rebases onto `main` after this merges.

**Depends on:** nothing.

**Status:** done. Unblocks branch 2.

**Outcome:**

* Added `rpcAbandon(id)`: gives up on a request and releases the claim;
  `rpcFinish` drops its late reply by id. `rpcCall` uses it on timeout,
  `probeTick` on a leg deadline, so the restore's calls queue behind a leg
  Core 1 is still running instead of failing busy.
* `rpcProbeLegPost` is renamed `rpcProbeLegStart`; `rpcStepDebug` returns an
  `RpcResult` (its callers still ignore it). `rpcFinish` keeps the live
  request's cmd and node itself, so its signature is `(id, out)` as planned.
* `rpcBusy` kept, still unused. `RPC_PENDING` prints as `pending`.
* For branch 2: every request passes through `rpcStart`, so the exclusion
  check goes there; `RPC_EXCLUDED` joins the enum beside `RPC_PENDING`.
* Checks: `pio run -e pico` clean. No automated test (no `ipc/` harness).
  Human scope open, as listed in Checks.

## Branch 2: `feature/bus-sweep`

**Type:** feature. New alarm reason, commands, replies and boot behaviour.

**Purpose:** the Decisions under "Bus sweep", "Slots are freed only by
confirmation: the fence" and "Estop", except strictness and session endings
(branch 4).

**Settled in planning (revised after 1a and 1b):**

* Make safe is `CMD_MAKE_SAFE`, confirmed by its status reply (Decisions).
  One helper decides "released" from a status reply (stepper tail slot, the
  probe vacuum's third tail byte, no slot byte = released); `cmdMakeSafe`
  (`core0/cmd/periph.cpp`) moves onto it.
* Core 1 keeps only `nodeEnabled`, already folded from status replies by
  `noteEnabled` (`core1/rpc_server.cpp:96-107`); no change there.
* The fence is a flag beside `slotNode[]` (`core0/ops/position.cpp:17`). Touched
  is `nodeEnabled` or holding a slot.
* `mute` and `excluded` are Core 0 masks in a new `ops/bus.*`, beside
  `busSweep()`, which the boot sequence calls. Participants (config nodes not
  mute) are derived, for `status` only.
* The sweep runs from Core 0 as RPCs (`rpcNodeStatus(CMD_MAKE_SAFE, …)`, as
  `makesafe` does), after the wipe releases Core 1. Worst case 8 ×
  `RESPONSE_TIMEOUT_MS` (20 ms) = 160 ms. With no config it still runs; only
  touched nodes can then be mute.
* Exclusion is checked once, in `rpcStart` (branch 1e), which every request
  passes through: a node-addressed request to an excluded node returns a new
  `RPC_EXCLUDED` (`err excluded`). Make-safe is exempt.
* `unstop` waits only on nodes that are not excluded.
* Until branch 4, `unalarm` in `ALARM_ESTOP` answers `err estop` (its map
  retry would leave the estop unconfirmed).

**Settled in planning (revised after 1d):**

* The slot rules (park, fence, `-`, `err fenced`, the `!` readback, the fence
  retry) go in `ops/slot_map.cpp`; the fence flag beside `slotNode[]` in
  `position.cpp`; `ops/bus.*` holds only the sweep and the `mute`/`excluded`
  masks. `axes_map` and the probe inherit the rules by applying through
  `slotMapCommit`.
* `err degraded` refuses `slot_map`, `axes_map` and `probe`.
* Confirmed `makesafe <id>` drops the node from both requests: the slot
  request, and for an axis node the axes request (`-`, not pending).
* A pending axis keeps its id in the slot request, and `slotMapCommit`'s
  `parkOnly` flag becomes a skip mask: skipped slots are parked, not engaged,
  so the request stays unmet and `NODE_FAULT` follows from `slotMapComplete`
  alone (Decisions). Replaces 1d's "pending parks every holder".
* `axes_map`'s type check uses the fence retry's make-safe reply for a fenced
  node, and `CMD_NODE_STATUS` for every other node. Read confirms the
  make-safe reply carries the node type.
* `axisNode(k)` is `-` for a fenced slot, so 1d's unbound-axis refusal in
  ingest (`data_plane.cpp:180`) covers fenced slots with no new check.
* With `CFG_SET` going through reset, `keepWrongType` is used only by the boot
  default map; the Outcome records it.

**Files** (under `src/rp2350/` unless noted):

1. Release rule, fence, estop, `makesafe`:
   * `core0/ops/position.cpp:13-90`: the fenced flag beside `slotNode[]`
     (`:17`); `slotMapReset` (`:73`) keeps ids and fences; `axisNode` (`:87`)
     unbinds a fenced slot; `reconcileValidity` (`:295`): fence every bound
     slot on the rising edge of `STATE_ALARM` + `ALARM_ESTOP`.
   * `core0/ops/slot_map.cpp:20-68` (`applySlots`): a park or engage with no
     confirmation fences the slot; an engage into a fenced slot first retries
     make-safe on its node; only confirmed slots are freed (the failed-engage
     path included); `err fenced`. `slotMapComplete` (`:94`): a fenced slot
     satisfies `-`. `cmd/axis.cpp` (`cmdSlotMap`): readback marks `!`.
   * `core0/ops/axes_map.cpp:14-87`: `checkPending` takes the fence retry's
     reply as the type check for a fenced node; `axesCommit` requests `-` for a
     pending axis instead of parking every holder.
   * `core0/core0.cpp:113`: the wipe's `slotMapReset()` keeps the slot table.
   * `core0/ops/probe.cpp`: bind and restore go through `slotMapCommit` and
     `axesMapApply`; checked, not changed.
   * `core1/bus/packet.cpp:65-88`: `busDisableAll` becomes `busMakeSafeAll`;
     its CONCERN comment is answered by `unstop`.
   * `core1/core1.cpp:44-80` (estop): broadcast disable, then `busMakeSafeAll`;
     fix the stale `axes_enabled` comment. `:160` (reset park): no sweep.
   * `core0/cmd/periph.cpp` (`makesafe`): unbind and drop from both requests
     on confirmation, fence otherwise; the shared "released" helper.
2. `unstop`: `core0/cmd/lifecycle.cpp`, `core0/cmd/table.h`;
   `core0/controller/cmd/unalarm.cpp` (`err estop`).
3. Sweep, degraded, `bus_exclude`:
   * New `core0/ops/bus.{h,cpp}`: `busSweep()`, `mute`, `excluded`, touched.
   * `core0/core0.cpp:146-149`: banner, `busSweep()`, the default map unless
     degraded, then `ready`.
   * `ipc/core1_rpc.cpp` (`rpcStart`): the exclusion check, with make-safe
     exempt; `RPC_EXCLUDED` and its text.
   * `ipc/shared_state.h`: `ALARM_BUS_DEGRADED = 8`.
   * `core0/ops/state.cpp`: `resumeOrHold` settles `ALARM_BUS_DEGRADED` first.
   * `core0/cmd/` + `cmd/table.h`: `bus_exclude`; `slot_map`, `axes_map`
     (`cmd/axis.cpp`) and `probe` answer `err degraded` in
     `ALARM_BUS_DEGRADED`.
   * `core0/cmd/query.cpp`: `status` reports mute, excluded and touched (text
     plane only; STATUS_RSP layout unchanged).
4. `CFG_SET` → reset: `core0/data_plane.cpp:245` sends the ACK, then raises
   `soft_reset_requested` instead of `controllerApplyDefaultMap()`.
5. Web: deferred with 1d's (see 1d, Web deferred). When ported:
   `src/wire/link/link.ts:212-226` (`pushConfig`) waits for `ready` after
   `CFG_ACK` (with a timeout, not a desync, banner drained);
   `src/wire/format/status.ts` (`BUS_DEGRADED`); `commands.ts` (`unstop`,
   `bus_exclude`, `err fenced`); `sim.ts` (`stop`, `unstop`, `bus_exclude`).
   A host reads `axes_map`, so the `!` readback stays console-only.
6. Docs: `docs/engage_and_axis_map.md` (release rule, fence, estop,
   `makesafe`; §5.4's pending rule becomes per slot), `docs/wire_protocol.md` (commands, errors, readback, `ready`),
   `docs/config_storage.md` (reset after commit),
   `docs/node_type_architecture.md` (a hung `loop()` is not caught by the
   silence timeout; see Follow-ups).

**Out of scope:** strictness, the `unalarm` dispatcher, session endings on
estop and `claimed` (branch 4); `setorigin` (branch 3; it still clears
`ALARM_ESTOP` until then); the node-side claim; a node hardware WDT.

**Commit units (proposed):** 1 release rule, fence, estop make-safe and
`makesafe`; 2 `unstop`; 3 sweep, degraded, `bus_exclude`; 4 `CFG_SET` reset
and `ready`. Docs travel with the unit they describe.

**Checks:** `pio run -e pico`, `pio test -e native`. Human scope: estop with
a node unplugged, `reset` with a mute
touched node, the walkthrough (fence, `-`, fence retry, degraded, exclude), a
head switch keeping its datums, `CFG_SET` then `ready`, sweep timing on the
bus.

**Overlap:** none (`irq-bench` has no branch type). Touches
`core0/cmd/table.h`.

**Depends on:** branches 1, 1b, 1d (merged), 1e. "Holds a slot" in touched
includes the probe vacuum with no special case.

**Read findings (second Read, after 1a/1b/1c):**

* The probe vacuum is outside the slot table and never released: moved to
  branch 1d.
* `bus_enable off` (`cmd/axis.cpp:92`) broadcasts `CMD_DISABLE` and Core 1
  clears all of `nodeEnabled` on send (`core1/rpc_server.cpp:132`), unconfirmed.
  Dropped in Write: it stays a plain broadcast (under-claiming armed is the
  safe direction).
* A fenced node confirming an engage into another slot also clears the fence
  on its old slot (the reply shows its slot).
* Late replies: `busQuiesce` flushes RX before every transaction and
  `receivePacket` filters node and opcode; node `loop()` has no blocking call
  but the boot blink. A late `CMD_NAK` passes the opcode filter but is too
  short to confirm a status command.
* `step` needs no fence check: `step <node>` resolves the node's own slot
  (`cmd/axis.cpp:411`), and a fenced slot holds no other node.
* For the Outcome, out of scope: `receivePacket` (`core1/bus/packet.cpp:24-38`)
  writes `rxBuf[32]` without a bound; a garbage length byte on a noisy bus
  overruns it.
* Cold boot never sweeps today (Core 1 sweeps only on leaving its loop,
  `core1.cpp:160`); the Core 0 sweep covers it.

**Read findings (first Read, before 1a/1b were split out):**

* `core1.cpp:160`: the reset park's `busDisableAll()` runs during the wipe;
  with the sweep on Core 0 after release, the park only parks.
* The host has no banner detection; a banner arrives as an unrequested text
  line and the next `command()` drains it as a desync.

**Status:** done (9709472..c6e3723). Unblocks branch 4 (with 3); branch 3 was already unblocked.

**Outcome:**

* Deviations:
  * The estop sweep is `busStopAll(cmd)` (`core1/bus/packet.cpp`), called
    with `CMD_MAKE_SAFE`; the reset park no longer sweeps (Core 0's boot sweep
    replaces it).
  * A failed fence retry does not stop the map: the fenced slot stays unmet
    and the other slots apply, as for a pending axis. A failed engage still
    stops the apply at that slot.
  * An engage answered with a NAK does not fence (a confirmed refusal).
  * One reply line per map: `err fenced` over the pending error over an
    engage error.
  * A make-safe reply still showing `NODE_FLAG_ENABLED` is unconfirmed, for
    `unstop` and the sweep (so such a node can be mute).
  * `unstop` is a primitive (main command table), usable without a config.
  * The exclusion mask lives in the RPC layer (`rpcSetExcluded`), kept in
    step by `ops/bus.cpp`, so `ipc/` does not depend on `core0/ops`.
  * `probe_map` answers `err degraded` (it is the `probe` entry).
* Interfaces later branches rely on:
  * `slotFence`, `slotFencedAt`, `axesReqDrop` (`position.h`);
    `slotMakeSafe`, `nodeStatusSlot`, `slotMapDrop`, `slotMapPrintFenced`,
    `slotMapCommit(req, fromAxes, skip, &res, &fenced)` (`slot_map.h`).
  * `busSweep`, `busDegraded`, `busMute`, `busExcluded`, `busTouched`,
    `busExclude` (`ops/bus.h`); `RPC_EXCLUDED`; `ALARM_BUS_DEGRADED = 8`.
  * `ready` ends every boot sequence, `CFG_SET` included.
  * `keepWrongType` is used only by the boot default map.
* Checks: `pio run -e pico` clean. `pio test -e native` not required
  (`lib/motion/` untouched); in this worktree the parity fixtures
  (`test/data/*_ref.txt`, gitignored, generated from `web/`) are absent, and on
  `main` 1 of 5 fails (`discretize` size): the fixtures need regenerating.
* Out of scope, found:
  * `receivePacket` (`core1/bus/packet.cpp`) writes `rxBuf[32]` without a
    bound; a garbage length byte overruns it.
  * The web host desyncs on a config push until item 5 is ported: it does
    not wait for `ready` after `CFG_ACK`.
  * Follow-up "stale comment in `core1.cpp`'s estop path" is fixed here.
  * `status` gained `mute=`, `excluded=`, `touched=`; any host parser of
    that line needs to allow extra fields.

## Open questions

* **Node side** (its own session): docs/node_session_and_datum.md §3 and §7
  already design most of it. `SET_SESSION` claims a node with a Pico-chosen
  token and self-safes it (disengage, de-energise, laser off): that is the one
  command every node supports, and ping → claim → engage is the boot sweep.
  A token of 0 means "never configured by this Pico", so the touched set shrinks to the
  nodes that do not answer the claim. Also there: detecting a node that reboots
  mid-job. Still open beyond it: a node unbinding itself after bus silence,
  which closes the last hole (a mute node holding a slot the Pico has
  forgotten): branch 1b's silence timeout covers it for a node cut off from
  the Pico, not for one that hears but cannot answer. That doc's §2
  (node-frame datum) is built, and its §6 and "no NAK"
  prerequisite are superseded.
* **Silence timeout, deferred (1c).** A flaky bus with outages of
  `BUS_SILENCE_MS` or more makes nodes safe (de-energised, un-homed, slot
  dropped) while the Pico still believes them bound and enabled; mid-job it
  keeps streaming to them. An idle round-robin `CMD_NODE_STATUS` in place of
  the zero-byte keepalive would catch it, since the reply goes through
  `noteEnabled` and shows the dropped slot, but each poll holds Core 1 up to
  `RESPONSE_TIMEOUT_MS` ahead of an operator command. Mid-job detection is
  open (a status check at job end, or `SET_SESSION`). `busstat` shows a flaky
  bus; `BUS_SILENCE_MS` is compile-time.
* **`alarmReason` has two writers.** Core 1 writes `ALARM_ESTOP`
  (`core1/core1.cpp`) and `ALARM_SOFT_LIMIT` (`core1/emit/microsegment.cpp`);
  Core 0 writes the rest (`ops/`, and the wipe in `core0.cpp`). Safe only
  because the writes never overlap in time. "Only ops write state" covers Core
  0's side; worth a header note (docs/node_state_ingest.md §7).
* Keying datum invalidation on the alarm reason: `originKillGen`
  (docs/node_state_ingest.md §7) becomes worth building when soft limits give
  Core 1 a second path that ends motion abruptly. Not needed here: the estop
  keeps `ALARM_ESTOP`.
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
* Optional node hardware WDT (`NODE_ENABLE_HW_WDT`), fed from `loop()`: a
  hung `loop()` keeps its RX ISR stepping and is caught neither by the silence
  timeout nor by the Pico (the fence covers the slot, not the node). Not
  planned; documented for whoever wants it.
* `bus_exclude` of any node, not only mute ones (drops `err not_mute`). A
  mute node is excluded as now (the sweep already made it safe and fenced
  its slots). A responsive node gets a make-safe first: untouched, excluded
  without waiting for the reply; touched (`busTouched`), a confirmed release
  unbinds its slots, otherwise they are fenced (`slotMakeSafe`) and the reply
  names it (`ok unconfirmed <ids>`). Either way the node is excluded.
  Exclusion blocks RPCs only, so an excluded node gets no slot again until
  the next sweep. Its own small branch.
