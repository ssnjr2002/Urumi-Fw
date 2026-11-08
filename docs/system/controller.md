# Controller

**Checked against `45350e2`.** To see what may have changed since:

```bash
git diff 45350e2 -- src/rp2350/core0/ops/machine.h src/rp2350/core0/ops/machine.cpp src/rp2350/core0/ops/sequence.h src/rp2350/core0/ops/sequence.cpp src/rp2350/core0/ops/runner.h src/rp2350/core0/ops/runner.cpp src/rp2350/core0/control_plane.cpp src/rp2350/core0/core0.cpp src/rp2350/ipc/shared_state.h
```

Commands, replies and status keys are written as the
[notation](../README.md#notation) says.

The controller is the program on Core 0 of the RP2350. It takes the host's
commands and job records, decides whether each may run, runs it, and
publishes one state that says what the machine is doing. Core 1 emits the
steps and drives the bus: Core 0 decides, Core 1 moves.

This page is the controller's model of the machine: what runs, what is wrong,
what the host is told, and when a command may run. What each command does is
in [commands](../reference/commands.md).

## 1. Hardware

| Part | What the controller does with it |
| --- | --- |
| A Raspberry Pi Pico 2 (RP2350), on the arduino-pico core | Core 0 runs the controller; Core 1 runs the steps and the bus |
| USB | its only link to the host: one CDC serial port |
| An RS485 transceiver, half duplex at 921600 baud | the bus to the nodes; Core 1 drives it through a PIO state machine (TX, RX and driver enable) |
| Flash | the stored config, `/config.bin` |
| The nodes: one small AVR board per axis or device | every motor, switch and sensor is on a node; the controller has no I/O pins of its own beyond the bus |

A node is a stepper (X, Y, a head's Z or A, with its limit switch or index
sensor), a vacuum (the pump, the probe switch, the servos) or a knife (the
oscillator, the blower). A laser hangs off a node. The cores, the bus and the
nodes each have their own page; how the firmware is built is
`firmware-build`.

## 2. I/O

**Host to Core 0,** over USB, in two planes:

* **Text commands** (`control_plane.cpp`): one line in, exactly one line out.
  The word is looked up in the primitive table, then the controller table; a
  controller command without a valid config answers `err unconfigured`; an
  unknown word answers `err unknown`. Each row is admitted before its handler
  runs (§5, Admission).
* **Binary records** (`data_plane.cpp`): job, tool and layer headers, BEZIER
  records, continuous jog and config transfer, answered by ACK or NACK. A
  record is admitted by the published state, not by a row.
* `STATUS_REQ` is answered at once, outside both planes.

Every command and every record ends by publishing the state (§4), so the next
one reads what this one left.

**Core 0 to Core 1,** over three channels:

* a synchronous RPC for node commands: the handler waits for the node's
  reply;
* shared flags: `estopRequested`, `abortRequested`, `pauseRequested`, the
  context and the job's reason;
* the planner ring: motion, which Core 1 turns into step bytes.

Core 1 reports back with facts (e-stop swept, ring drained, held, a tool's
power failed, duty held), which the controller reads first in every loop pass.

**Core 1 to nodes,** over RS485. Core 0 never touches the bus. Its node
commands go out as RPC packets; motion goes out as stream bytes, one
per step. When nothing else is sent for 333 ms, Core 1 sends an empty stream
byte, the keep-alive. A node built with a silence timeout that hears nothing
for 1 s makes itself safe: it de-energises, drops its slot and forgets its
datum (§3, Faults).

**Flash to Core 0:** the stored config, loaded at boot and on `reset`. The
controller table needs a valid one; `F_CONFIG` is held while there is none.

## 3. Lifecycle

### Boot

Power-on and `reset` run the same sequence:

1. Core 1 parks; USB input and the channels are flushed.
2. The state is wiped: no faults, no e-stop, nothing running, position zero,
   nothing homed. The stored config is reloaded, dropping unsaved edits.
3. Core 1 is released.
4. The bus sweep makes every node safe and records which nodes are mute.
5. With no mute node, the default head's axis map is applied.
6. `ready`.

`reset` restarts the loop, not the chip: USB stays up.

### Levels

What runs is three nested levels, under the faults and the e-stop latch:

```text
faults, e-stop latch     override everything
  context                where the machine is; held between commands
    sequence             what was asked for
      step               what moves now
```

* **Context:** IDLE, JOB, PROBE or HOMING.
* **Sequence:** a command's work, as a series of steps: `home`, `recovery`,
  `goto`, `tool change`, `place`, and the one-step `leg` and `traverse`.
* **Step:** LEGS (node legs), RING (planner motion) or WAIT.

Each is in §5. Three rules hold them together:

* **Ownership goes down.** Closing or aborting a level tears down the levels
  below it: closing a context aborts its sequence, aborting a sequence cuts
  its step. A step never checks its owner.
* **Contexts never nest.** PROBE and HOMING open only from IDLE.
* **A sequence has no context of its own:** it runs in the one it started in.
  A sequence never starts another.

### Faults and the e-stop latch

`machineFaults()` is a 16-bit mask; its values are the wire's (`STATUS_RSP`,
`get faults=`).

| Bit | Name | Kind | Set when | Clears |
| --- | --- | --- | --- | --- |
| `0x0001` | `F_BUS` | held | a node the sweep found mute is not excluded | `bus_exclude`, or a `reset` whose sweep hears it |
| `0x0002` | `F_CONFIG` | held | there is no usable config | a config is adopted (or `uncfg`) |
| `0x0004` | `F_UNMAPPED` | held | the axis map is not met | it is met; `unalarm` retries it |
| `0x0008` | `F_LATCHED` | held | an axis stands on its switch | it is off it; `unalarm` starts `recovery` |
| `0x0010` | `F_HOMING` | event | a homing, recovery or traverse leg failed, or `leg_abort` stopped one; `homefail=` and `homenode=` say why | `unalarm`; the axis is not homed until homed again |
| `0x0020` | `F_PROBE` | event | a probe leg failed; the probe binding is restored | `unalarm` |
| `0x0040` | `F_FACTS` | event | Core 1's fact ring overflowed | `unalarm` |
| `0x0080` | `F_SEQUENCE` | event | a sequence's step or instant work was refused after its `ok`; `seqfail=` and `seq=` say which | `unalarm` |
| `0x0100` | `F_TOOL` | event | a tool's power switch failed | `unalarm` |
| `0x8000` | `F_ESTOP` | the latch | `stop` | `unstop` |

* **Held bits** refuse new work and tear nothing down. They are never stored:
  each read recomputes them from their causes, so they clear when the cause
  goes.
* **Event bits** tear down whatever runs as they are set (Teardown, below)
  and reset the tool's duty. They stay until `unalarm`.
* **The e-stop latch** is not a fault. `stop` latches it, raises
  `estopRequested` and tears down; Core 1 drops the ring, sweeps the bus safe
  and sets `estopSwept`. Every node's datum is lost. The machine shows ESTOP
  until the sweep and ALARM after, so ALARM always means the bus is parked.
  `unstop` makes safe every touched, unexcluded node and clears the latch only
  when all of them confirm.

**`unalarm`** clears the event bits, whose cleanup already ran, then fixes the
held bits in order and stops at the first it can't fix:

1. a mute node: `err degraded`;
2. an unmet map: retried once, else `err unmapped`;
3. an axis on its switch: starts `recovery` and answers `ok` once it runs.

`F_CONFIG` has no fix here. The reply carries the faults left. Under an
e-stop it answers `err estop`: the latch's way out is `unstop`.

**What the controller doesn't see:**

* **A node that resets or makes itself safe** loses its datum: the node
  clears its datum witness, and only the controller's datum command sets it
  again. The controller reads the witness each time it binds the node or ends
  a leg on it, and unhomes the axis when it is gone. Between those reads
  nothing asks, so a node that resets while a job streams to it, or while a
  `goto` or a jog runs, still counts as homed until its next bind or leg.
* **A bus outage longer than the silence timeout** makes the nodes safe while
  the controller still counts them bound and enabled; in a job it keeps
  streaming to them. `F_BUS` comes only from the boot sweep, so an outage
  later raises no fault.

### Teardown

`machineTeardown` runs on `stop` and on every event fault, top to bottom:

1. **Legs:** dropped on an e-stop (Core 1 is sweeping), aborted otherwise.
2. **The sequence:** aborted, so its `abort()` cleans up.
3. **The context:**
   * PROBE restores its binding and closes; on an e-stop it drops the binding
     without the bus.
   * HOMING closes.
   * JOB closes at once on an e-stop (the sweep drops the ring). Otherwise its
     abort starts: Core 1 brakes and drops the ring, the controller queues one
     lift, and the context closes once that has drained.
   * A jog in IDLE is stopped.
4. **Publish.**

## 4. Loop

### The ticks

Handlers never block on motion: a leg runs for seconds, and `get` and `stop`
must answer throughout. A command starts the work and replies; a tick carries
it on in later passes. A node command's RPC is the one wait inside a handler.

Each pass, in order:

1. **`observeTick`: Core 1's facts**, before the host is served, so a command
   reads what Core 1 last did:

   | Fact | What the controller does |
   | --- | --- |
   | e-stop swept | clears the fenced slots' positions |
   | ring drained | ends a RING step; a held job's lift is done, so it lands PAUSED |
   | held | queues the pause's lift and switches the tool off, or lands PAUSED at once with nothing to lift |
   | tool failed | sets `F_TOOL` |
   | duty held | lifts the tool until its duty resets |
   | facts lost | sets `F_FACTS` |

   Then it updates which axes are energised from the nodes' replies.
2. **`processSerial`: the host's input.** Every waiting USB byte: a binary
   record goes to the data plane, `STATUS_REQ` is answered, anything else
   builds a text line, run when it ends. Each command and record is
   published as it ends.
3. **`dataPlaneTick`: the data plane's upkeep,** once the input has drained:
   * sends the ACKs it held back to batch them;
   * continuous jog: stops a jog whose packets stopped for 150 ms (the
     deadman), and starts one that waited for the ring to empty;
   * drops a record half-received for 50 ms; the host resends it;
   * answers a config transfer stalled for 2 s with a NACK.
4. **`runnerTick`: one leg's poll** (§5, Step).
5. **`sequenceTick`: the live sequence's next move** (§5, Sequence).
6. **`toolChangeTick`: starts the `tool change` sequence** once the job has
   reached its tool change (§5, Sequence), and forgets it when the job closes.
7. **`placeTick`: starts the `place` sequence** once the job has reached
   placement. When placement ends it turns off the dot's window, and once the
   job closes it selects again the head that was selected before.
8. **`probeTick`: the probe leg's supervisor** (§5, Step).
9. **`machinePublish`**: what the pass left.

### Publishing state

`machinePublish` is the only writer of `machineState`, the state the host
reads in `STATUS_RSP` and `get state=`. No logic reads it back: the controller
asks the levels instead. The first rule that matches wins:

| Rule | State |
| --- | --- |
| e-stop latched, not yet swept | ESTOP (2) |
| e-stop latched and swept | ALARM (3) |
| a `home` or `recovery` sequence | HOMING (5) |
| a `goto` sequence | JOGGING (7) |
| a `tool change` or `place` sequence | JOB (9) |
| the PROBE context | PROBING (6) |
| the HOMING context | HOMING (5) |
| a LEGS step | TRAVERSE (8) |
| a RING step not owned by the job | JOGGING (7) |
| the JOB context | JOB (9) |
| any fault | ALARM (3) |
| else | IDLE (0) |

A sequence is named whatever its context; a manual context keeps its name over
its raw legs; a step outranks the job context, so a jog in a paused job is
JOGGING. Beside the state the host reads the reason that refines it: the job's
reason, the homing and probing reasons, and the ring's owner.

## 5. Context

A context is held between commands; IDLE is the absence of the others.

| Context | Opens | Closes |
| --- | --- | --- |
| IDLE | | |
| JOB | the first record (`JOB_HDR`), from IDLE | `cancel` at rest; an abort, once its lift has drained; an e-stop; an event fault |
| PROBE | `probe_map` | `probe_end`; an e-stop; an event fault |
| HOMING | the first raw seek, retract or sweep `leg` | `setorigin`; `home_end`; an e-stop; an event fault |

Inside JOB, the job's reason says where the job is: preflight, placement
(`JOB.PLACE`), buffering, running, holding, paused, dwelling before a plunge,
aborting, changing tools, or waiting for the operator to mount and zero one
(`JOB.TOOL_WAIT`).

### Step

| Step | What moves | Ends |
| --- | --- | --- |
| LEGS | node legs in parallel, a thread of legs per node | the last leg ends and no thread is open |
| RING | planner motion, with the ring's owner: the job, a step jog (`jog`, `line`, `bez`), a continuous jog, or a sequence | Core 1 reports the ring drained |
| WAIT | nothing | its timeout, or the event its command posts |

Legs and the ring never move at the same time. A step is not stored: it is
read from its owners (the runner's legs and a probe leg; the planner ring and
Core 1 running it; the sequence's wait).

**How a LEGS step runs.** A leg is one move a node runs by itself, ended by
its switch, its index or its target (see [homing](../machine/homing.md) §3).
The runner holds up to eight, one thread of legs per node; a node's next leg
may be armed when its last one ends. `runnerTick` polls one leg per pass, each
node at most every 25 ms, with a status request:

* the node still stepping: running, unless the controller's own deadline for
  it has passed (a failure);
* the node stopped: the leg's verdict, from the status: on the switch or off
  it, the index found, the target reached with the datum witness intact;
* four polls unanswered: a failure.

Each end has its effect on the machine applied (`leg_effects`: a position, an
unhomed axis) and is posted to the runner's mailbox; the runner never calls
back. When no leg runs and no thread is open, it posts the step's end.

**How a RING step runs.** The controller queues the motion in the planner
ring and Core 1 runs it. The step ends when Core 1 reports the ring drained,
read by `observeTick`.

**How a WAIT step runs.** `sequenceTick` ends it at its timeout.

**A probe leg** is not the runner's. It is one RPC that Core 1 runs to its
end, stepping Z and polling the vacuum's bed switch. `probeTick` waits for its
reply: a failed cause, an unreadable reply, or a retract that left the switch
open sets `F_PROBE`; past its deadline the RPC is abandoned, with the same
fault.

### Sequence

| Sequence | Started by | Runs in | Published as |
| --- | --- | --- | --- |
| `leg` | a raw `leg` in the HOMING context: one step | HOMING | HOMING |
| `traverse` | a `leg` that keeps the origin (park, span, unlatch): one step | IDLE | TRAVERSE |
| `home` | `home` | IDLE | HOMING |
| `recovery` | `unalarm` with an axis on its switch | IDLE | HOMING |
| `goto` | `goto`; `mount` and `unmount` on a head with a holder lock | IDLE; `goto probe`, `goto mount`, `mount` and `unmount` also in `JOB.TOOL_WAIT` | JOGGING |
| `tool change` | a tool or layer header whose tool is not mounted and zeroed on its head | JOB | JOB |
| `place` | every job, once its last tool header is in | JOB | JOB |

**How a sequence runs.** Its command, or its tick, arms the first step, then
starts it. Each pass, `sequenceTick` pops the runner's mailbox:

* a failed leg fails the sequence (`F_HOMING`);
* a leg that succeeded goes to the sequence's `leg()`, which may arm that
  node's next leg in the same step;
* the end of a step, or of a wait, goes to `next()`, which runs any instant
  work (reads, datum commits) and arms the next step, waits, or ends the
  sequence.

A RING step's end comes from `observeTick` and a wait's from `sequenceTick`'s
clock; both go to `next()` the same way. An abort cuts the step and calls the
sequence's `abort()` to clean up.

**How a job reaches its sequences.** `tool change` and `place` are not
started by a command:

* **`place`:** the last tool header moves the job from preflight to
  placement; `placeTick` starts the sequence on the next pass.
* **`tool change`:** a header whose tool isn't ready asks the planner to hold
  for it. A job at rest moves to its tool change at once; a moving job
  finishes the motion already queued, and Core 1 moves it there when the ring
  drains. `toolChangeTick` then starts the sequence, which parks the Zs,
  unlocks the head's A and takes the head to `mount`, and leaves the job in `JOB.TOOL_WAIT`.

### Admission

Each row of the command tables has a kind. `machineAdmit` checks it before the
handler runs, and answers `busy` when something running is the reason,
`bad_state` otherwise. "Something running" is a live step (a paused job's
parked ring excepted) or a live sequence.

The kind also says what the command becomes:

| Kind | Becomes | Admitted when | Commands |
| --- | --- | --- | --- |
| `any` | nothing that runs | always | `stop`, `feed`, `leg_abort`, `bus_exclude` |
| `read` | nothing that runs | always | `get`, `?`, `nodestat`, `ping` |
| `write` | an instant change: a setting, a switch, a binding | nothing running, the e-stop swept or not latched, and the context IDLE, a paused job, or a manual context whose flag the row carries | `set`, `save`, `select`, `wzero`, `enable`, `laser`, `setorigin`, `reset` |
| `step` | one step, with no sequence; a `leg` outside PROBE becomes a one-step `leg` or `traverse` sequence | nothing running, no e-stop latched, not in a job | `jog`, `line`, `bez`, `leg` |
| `sequence` | a sequence | nothing running, no e-stop latched, the context IDLE, no fault but `F_LATCHED` | `home`, `goto`, `mount`, `unmount` |
| `context` | a context opened or closed, by the handler's rules | no e-stop latched | `probe_map`, `probe_end`, `home_end`, `pause`, `cancel`, `place` |
| `clear` | faults or the latch cleared | nothing running, the context IDLE | `unalarm`, `unstop` |

Flags on a row widen its kind:

* Inside PROBE or HOMING only `any`, `read` and the rows flagged for that
  context are admitted.
* In `JOB.TOOL_WAIT`, rows flagged `tool wait` are admitted when nothing runs
  and no e-stop is latched, whatever their kind; in `JOB.PLACE`, rows flagged
  `place`.
* A row flagged `bare read` skips the check when it has no arguments.
* `F_LATCHED` does not refuse a sequence: getting off the switch is the cure.

What a handler checks beyond its kind (its arguments, the bus, the context) is
in [commands](../reference/commands.md).

## 6. Who writes what

| Value | Written by | Read by |
| --- | --- | --- |
| The context | the module that owns it: the planner queue opens JOB and `cancel` or an abort closes it, under `plannerLock`; `ops/probe` opens and closes PROBE; `ops/homing` opens HOMING on the first raw leg, `setorigin` and `home_end` close it; the teardown closes any | admission, the published state, Core 1 (it runs the job's ring only while JOB is open) |
| The job's reason (`jobReason`) | Core 0 for what it decides at rest (preflight, tool change, placement, the landing in PAUSED); Core 1 for what it decides from the ring (running, holding, aborting) | admission, the published state, the data plane |
| The live sequence | its command starts it (`sequenceStart`); it ends itself, or the teardown aborts it | the loop (`sequenceTick`), admission, the published state |
| The live step | not stored: read from its owners (§5, Step) | admission, the published state |
| The ring's owner (`ringOwner`) | Core 0, when it restarts an empty, idle ring | the published state, the wire's JOGGING reason |
| Event faults | whatever detects them (§3, Faults) | `machineFaults`; `unalarm` clears them |
| Held faults | never stored: recomputed on every read from their causes | `machineFaults` |
| The e-stop latch | `stop` sets it; `unstop` and the soft reset clear it | everything |
| `estopRequested`, `estopSwept` | Core 0 sets the request with the latch; Core 1 sets `swept` when the bus is safe | Core 1 (every emitter stops on the request), the published state |
| `machineState` | `machinePublish` only | the host. No logic reads it |

## 7. Decisions

* **A fault mask, not one alarm reason.** One state byte and one reason byte
  held what runs, what is wrong and what the host is told; they could not
  hold two faults, nor a probe inside a paused job, so the code re-derived the
  state from guesses. Rejected: a richer reason enum, which still holds one
  fault.
* **Held faults are never stored.** Recomputed from their causes, they cannot
  go stale and need no clearing.
* **The e-stop is a latch, not a fault,** published as ALARM only once Core 1
  has swept the bus, so ALARM always means the bus is parked.
* **Three levels, ownership down.** A teardown is one walk down the levels.
  Rejected: a mode on each primitive, writing state when run alone and
  returning a result inside a sequence; every operation would carry two
  endings that drift apart. A primitive has one ending; only who reads it
  differs.
* **One writer of the published state, and nobody reads it.** Logic asks the
  levels, so a state computed for the host never feeds back into a decision.
* **Admission is a column of the table.** The kind's rule runs before the
  handler; only argument checks stay in handlers. Rejected: a state check in
  each handler, which is how the matrix and the code drifted.
* **Ends are posted, never called back.** The runner's mailbox is read on the
  loop's tick by the one reader, the live sequence; an aborted step clears it,
  so no end outlives its step.
* **Two command tables.** Primitives are atomic and config-free, everything
  they need an argument; controller commands read the config and have one
  gate, `err unconfigured`.
* **Core 1 keeps the job's reasons it decides from the ring** (running,
  holding, aborting): they must be true before anyone can observe them.
* **The datum is a conjunction.** An axis is homed only while the controller
  holds an origin for its node and the node's witness says nothing has
  interrupted it since. The node's half covers what the controller never
  sees: a brownout, a watchdog reset, a de-energise it didn't issue.

## 8. Code

All under `src/rp2350/`:

| File | Holds |
| --- | --- |
| `core0/core0.cpp` | setup, the loop, boot and soft reset, line assembly |
| `core0/control_plane.cpp` | the two command tables and dispatch |
| `core0/data_plane.cpp` | the binary records |
| `core0/ops/machine.h`, `machine.cpp` | faults, the latch, contexts, the step, admission, teardown, publish |
| `core0/ops/sequence.h`, `sequence.cpp` | the sequence level and its driver |
| `core0/ops/runner.h`, `runner.cpp` | legs in steps, the mailbox |
| `core0/ops/leg_effects.h` | what a leg's end does, by its intent |
| `core0/ops/observe.cpp` | Core 1's facts |
| `core0/ops/position.cpp` | origins, and reading the node's datum witness on bind |
| `core0/ops/homing.*`, `traverse.*`, `probe.*` | the homing context and legs, traverse legs, the probe context |
| `core0/controller/seq/` | the `home`, `recovery`, `goto`, `tool change` and `place` sequences, the default map |
| `core0/controller/cmd/unalarm.cpp` | `unalarm` |
| `core0/cmd/lifecycle.cpp` | `stop`, `unstop`, `reset`, `pause`, `cancel` |
| `ipc/shared_state.h` | the published state, context, reasons and ring owner enums |

The layers point down only: `cmd/` (primitives) → `ops/`;
`controller/cmd/` → `controller/seq/` → `ops/` and `config/`. Operations
print nothing: they return a refusal (`ops/refusal.h`) and the handler
replies.
