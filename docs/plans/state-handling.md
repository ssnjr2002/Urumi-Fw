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
  cycle. `axis_map` is refused while degraded (`err degraded`): the mute node
  is decided first, and nothing could run on the map anyway.
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
  slot; the **request** (`axis_map`'s argument) says what is wanted. A fenced
  slot always keeps its node id: nothing writes `{-, fenced}`.
* **A slot is fenced** when a node holding it, or being engaged into it, does
  not confirm: `axisMapApply`'s park and failed engage, the probe restore
  (through `axisMapApply`), `makesafe` with no reply, and, on the estop edge,
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
  `makesafe`, `unstop`, or `axis_map` retrying it, below), or a power cycle.
  Never on time or silence: the silence timer is fed by any byte that passes
  the FERR check, while a park needs a whole frame with a good CRC, so a node
  can keep missing its park and still be fed.
* **`axis_map` into a fenced slot** (for its own node or another) first sends
  make-safe to the fenced node. Confirmed: the fence clears and the engage goes
  ahead. Unconfirmed: the map fails, `ALARM_NODE_FAULT`, with
  `err fenced <s0> <s1> <s2> <s3>` naming the fenced node of each slot the
  request collides with (`-` elsewhere), e.g. `err fenced - - 3 -`.
* **Readback:** `axis_map` with no argument prints the binding with fenced
  slots marked, `1 2 3! 4`, and the request beside it when they differ.
* **`makesafe <id>`**: confirmed, the node's slot is unbound and dropped from
  the request, so no alarm follows; unconfirmed, the slot is fenced
  (`NODE_FAULT`).
* **The slot table survives the wipe**, bindings and fences both. It records
  what the nodes may still be doing, so only power-on clears it. The wipe
  clears positions, homed bits and the requested map; the sweep then frees
  every slot whose node confirms. A node that died for good keeps its slot
  fenced until a power cycle; `reset` and `CFG_SET` do not clear it.
* **`nodeReleased` is not needed:** a node is released when it holds no slot,
  bound or fenced.
* **A head switch keeps its datums.** `axis_map` parks with a disengage only
  (`ops/axis_map.cpp:38`), never a disable, so parked and re-engaged nodes
  keep their witness (`parkRecord`/`parkMoved`). Only make-safe (estop, the
  boot sweep, `unstop`, `makesafe`, a fence retry) costs the homing. A park
  that is not confirmed fences the slot and ends the switch in `NODE_FAULT`.

### Walkthrough

X = node 1, Y = 2, Z = 3, A = 4 in slots 0..3; vacuum = 5. Homed, IDLE.

```
node 3's cable works loose            nothing polls; still IDLE
axis_map 1 2 3 4 (head change)        1,2,4 park and re-engage, keep datums;
                                      3 silent: slot 2 fenced, 1 2 3! 4
                                      -> ALARM_NODE_FAULT, err fenced - - 3 -
  axis_map 1 2 - 4                    fenced slot 2 satisfies - -> IDLE
                                      Z steps NACKed; X, Y, A keep datums
  cable fixed, axis_map 1 2 3 4       make-safe to 3 confirmed, fence cleared,
                                      3 engaged -> IDLE; home Z
  or reset                            wipe; sweep: 1,2,4,5 answer, slots
                                      0,1,3 freed; 3 mute (holds slot 2)
                                      -> ALARM_BUS_DEGRADED, no map
     bus_exclude 3                    -> IDLE unmapped; slot 2 still 3!
     axis_map 1 2 - 4                 -> IDLE; cmds to 3: err excluded
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

All touch `web/src/wire/` and the Sim (`web/src/wire/link/backends/sim.ts`).

* Alarm reasons: `ALARM_CONFIG` retired; `ALARM_BUS_DEGRADED` added.
* Commands: `unstop`, `bus_exclude`, `home_end`, `cfg`; `setorigin` syntax.
* Errors: `err excluded`, `err degraded`, `err fenced <s0> <s1> <s2> <s3>`, `err node <id> …`
  from `setorigin`.
* `axis_map` readback marks fenced slots (`3!`) and shows the request beside
  the binding when they differ.
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
2. `feature/bus-sweep`: boot sequence sweep, mute and touched nodes, degraded bus
   and `bus_exclude`, the fence, `CFG_SET` → reset and `ready`, confirmed slot
   release, estop make-safe, `unstop`. Make safe is one `CMD_MAKE_SAFE` per
   node. Depends on 1 and 1b.
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
* Exclusion is checked once, in Core 0's RPC call path: a node-addressed call
  to an excluded node returns a new `RPC_EXCLUDED` (`err excluded`).
  Make-safe is exempt.
* `unstop` waits only on nodes that are not excluded.
* Until branch 4, `unalarm` in `ALARM_ESTOP` answers `err estop` (its map
  retry would leave the estop unconfirmed).

**Files** (under `src/rp2350/` unless noted):

1. Release rule, fence, estop, `makesafe`:
   * `core0/ops/position.cpp:14-77`: the fenced flag beside `slotNode[]`;
     `:69` (`axisMapReset`'s clear) keeps ids and fences; `:234`
     (`reconcileValidity`): fence every bound slot on the rising edge of
     `STATE_ALARM` + `ALARM_ESTOP`.
   * `core0/ops/axis_map.cpp:18-87`: a park or engage with no confirmation
     fences the slot; an engage into a fenced slot first retries make-safe on
     its node; only confirmed slots are freed (the failed-engage path included);
     `err fenced`. `axisMapComplete` (`:107`): a fenced slot satisfies `-`.
     Readback marks `!`.
   * `core0/core0.cpp:113`: the wipe's `axisMapReset()` keeps the slot table.
   * `core0/data_plane.cpp:136-170`: ingest refuses steps for a fenced slot,
     after the deltas are decoded, before the enqueue (`MSEG_NACK_BAD_STATE`).
   * `core0/ops/probe.cpp`: the restore goes through `axisMapApply`; checked,
     not changed.
   * `core1/bus/packet.cpp:65-88`: `busDisableAll` becomes `busMakeSafeAll`;
     its CONCERN comment is answered by `unstop`.
   * `core1/core1.cpp:44-80` (estop): broadcast disable, then `busMakeSafeAll`;
     fix the stale `axes_enabled` comment. `:160` (reset park): no sweep.
   * `core0/cmd/periph.cpp` (`makesafe`): unbind and drop from the request on
     confirmation, fence otherwise; the shared "released" helper.
2. `unstop`: `core0/cmd/lifecycle.cpp`, `core0/cmd/table.h`;
   `core0/controller/cmd/unalarm.cpp` (`err estop`).
3. Sweep, degraded, `bus_exclude`:
   * New `core0/ops/bus.{h,cpp}`: `busSweep()`, `mute`, `excluded`, touched.
   * `core0/core0.cpp:146-149`: banner, `busSweep()`, the default map unless
     degraded, then `ready`.
   * `ipc/core1_rpc.cpp` (`rpcCall`): the exclusion check, with make-safe
     exempt; `RPC_EXCLUDED` and its text.
   * `ipc/shared_state.h`: `ALARM_BUS_DEGRADED = 8`.
   * `core0/ops/state.cpp`: `resumeOrHold` settles `ALARM_BUS_DEGRADED` first.
   * `core0/cmd/` + `cmd/table.h`: `bus_exclude`; `axis_map`
     (`cmd/axis.cpp`) answers `err degraded` in `ALARM_BUS_DEGRADED`.
   * `core0/cmd/query.cpp`: `status` reports mute, excluded and touched (text
     plane only; STATUS_RSP layout unchanged).
4. `CFG_SET` → reset: `core0/data_plane.cpp:233-238` sends the ACK, then
   raises `soft_reset_requested` instead of applying the map. Web
   `src/wire/link/link.ts:212-226` (`pushConfig`): after `CFG_ACK`, wait on
   the text sink for `ready` (with a timeout), not counted as a desync; the
   banner before it is drained. The Sim has no `CFG_SET` (Follow-ups).
5. Web wire and Sim: `src/wire/format/status.ts` (`BUS_DEGRADED`),
   `src/wire/link/commands.ts` (`unstop`, `bus_exclude`, `err fenced`, the
   readback's `!`), `sim.ts` (`stop`, `unstop`, `bus_exclude` answering
   `err not_mute` since the Sim has no mute nodes). Tests beside each.
6. Docs: `docs/engage_and_axis_map.md` (release rule, fence, estop,
   `makesafe`), `docs/wire_protocol.md` (commands, errors, readback, `ready`),
   `docs/config_storage.md` (reset after commit),
   `docs/node_type_architecture.md` (a hung `loop()` is not caught by the
   silence timeout; see Follow-ups).

**Out of scope:** strictness, the `unalarm` dispatcher, session endings on
estop and `claimed` (branch 4); `setorigin` (branch 3; it still clears
`ALARM_ESTOP` until then); the node-side claim; a node hardware WDT.

**Commit units (proposed):** 1 release rule, fence, estop make-safe and
`makesafe`; 2 `unstop`; 3 sweep, degraded, `bus_exclude`; 4 `CFG_SET` reset
and `ready`. Web and docs travel with the unit they describe.

**Checks:** `pio run -e pico`, `pio test -e native`, `pnpm typecheck`,
`pnpm test`. Human scope: estop with a node unplugged, `reset` with a mute
touched node, the walkthrough (fence, `-`, fence retry, degraded, exclude), a
head switch keeping its datums, `CFG_SET` then `ready`, sweep timing on the
bus.

**Overlap:** none (`irq-bench` has no branch type). Touches
`core0/cmd/table.h` and `web/src/wire/`.

**Depends on:** branch 1 (merged), branch 1b (merged). Independent of 1c; 1c
goes first as the smaller branch.

**Read findings (first Read, before 1a/1b were split out):**

* `core1.cpp:160`: the reset park's `busDisableAll()` runs during the wipe;
  with the sweep on Core 0 after release, the park only parks.
* The host has no banner detection; a banner arrives as an unrequested text
  line and the next `command()` drains it as a desync.

**Status:** planned.

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
* Stale comment in `core1/core1.cpp`'s estop path: it says Core 0 clears
  `axes_enabled` on ALARM + `ALARM_ESTOP`; `reconcileValidity` now recomputes
  it from `nodeEnabled` every pass.
