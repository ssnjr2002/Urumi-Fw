# Machine model: faults, levels, sequences

`machineState` and `alarmReason` hold three things in two bytes: what runs,
what is wrong, and what everyone else is told. One byte cannot hold a probe
inside a paused job (`returnState`), nor two faults at once, so the code
re-derives the state from guesses (`resumeOrHold`, the "if in this alarm"
checks in `cmd/axis.cpp:231`, `ops/slot_map.cpp:179`).

This plan splits them: a fault mask, three levels for what runs, and a
published state computed from both once per loop pass. It is a redesign, not
a refactor: behaviour changes where the old encoding hid something (listed
per branch).

Supersedes docs/plans/sequences.md from R2a on (its Decisions on session,
admission and sequences, and branches R2a–R4). R1 (the fact ring, the
observer) is done and kept. Revises docs/plans/motion-sessions.md's States.

## Decisions

### Faults

* **`machineFaults` (u16) replaces `alarmReason`**, on the wire too.
* **Event bits** are set by whatever detected them and cleared by `unalarm`:
  `F_HOMING`, `F_PROBE`, `F_FACTS` (the fact ring overflowed).
* **Held bits** are never stored: recomputed each pass from their levels,
  clear when the cause goes: `F_BUS` (`busDegraded`), `F_CONFIG`
  (`machineCfgBlocking`), `F_UNMAPPED` (`!slotMapComplete`), `F_LATCHED`
  (`homingLatched`).
* **The estop is a latch, not a fault:** set by `stop`, its `swept` flag by
  `ESTOP_SWEPT`, cleared by `unstop`. ALARM/ESTOP is published only once swept,
  so ALARM visible still means the bus is parked.
* An estop or an event fault tears down what runs. Held bits only refuse new
  work.
* Fault details stay data: `failWhy`, `failNode`, the probe's cause.

### Levels

What runs is three nested levels, under the fault mask:

```text
  faults / estop latch   override everything (ESTOP, ALARM)
  context                where the machine is
    sequence             what was asked for
      step               what moves now (legs, a thread per node; or the ring)
```

Three rules:

* Ownership goes down: closing or aborting a level tears down the ones below.
* The published state is the most specific level with a name: a fault, else
  a named sequence (`home`, `recovery`), else a manual context, else the
  step's kind, else the job or IDLE.
* Admission asks the context whether the command is allowed (`bad_state`),
  then the levels below whether anything runs (`busy`).

(Not the stream slots of `slot_map`, which bind nodes to axes.)

```text
context    IDLE | JOB.reason | PROBE | HOMING     held across commands, closed by command
sequence   — | home | jogto | probe | toolchg | recovery
step       — | LEGS | RING (owner) | WAIT         the runner's live step, closes itself
```

* **Context** holds something between commands: a job (id, resume point,
  vacuum), a probe binding, a partial datum. It closes only by its command,
  an estop or a fault.
  * PROBE (`probe_map` … `probe_end`) and HOMING (raw `leg seek` …
    `setorigin`/`home_end`) are manual, for the bench and calibration, and
    open only in IDLE. Contexts never nest.
  * Inside a job, probing and parking are sequences only.
* **Sequence:** what was asked for. A small state machine: `next()` takes a
  step end and returns the next step, done, or a fault, running instant work
  in between (`home`'s sweep reads and datum commits); `abort()` cleans up
  (the probe restores its binding) and is called by the teardown.
  * Built from shared step builders (`parkZs()`, `lineTo()`, `probeLegs()`,
    `homeLegs()`); a sequence never calls another sequence.
  * A primitive that takes time (`leg`, `jog`, `line`, `probe_leg`) is one
    step with no sequence.
* **Step:** LEGS (node legs in parallel), RING (planner motion), WAIT (an
  event or a timeout). RING carries the ring owner: `JOB`, `JOG_STEP`,
  `JOG_CONT`, `SEQ` (J2's `joggingReason`, generalised). "Jogging" is RING
  with a JOG owner, in any context.
* **Ownership is the chain step → sequence → context.** The step belongs to
  the live sequence, or to the manual context when no sequence runs. Closing
  or aborting a level tears down the levels below it (`contextClose` aborts
  the sequence, `sequenceAbort` cuts the step), so a step never checks its
  owner.
* **A sequence has no context of its own:** it runs in the one it started in,
  which admits it. `home` runs in IDLE or `JOB.PAUSED`; the manual HOMING
  context is only for raw legs.

Examples:

```text
leg park 3                 IDLE         / —       / LEGS
jog x 10                   IDLE         / —       / RING JOG_STEP
jogto park, step 1         IDLE         / jogto   / LEGS
jogto park, step 2         IDLE         / jogto   / RING SEQ
the job cutting            JOB.RUNNING  / —       / RING JOB
jog while paused           JOB.PAUSED   / —       / RING JOG_STEP
probe while paused         JOB.PAUSED   / probe   / LEGS
home                       IDLE         / home    / LEGS
home while paused          JOB.PAUSED   / home    / LEGS
tool change, waiting       JOB.TOOL_CHANGE / toolchg / WAIT
manual probe leg           PROBE        / —       / LEGS
```

### Published state

```text
estop latched           → ESTOP (+ swept)
a step or a sequence    → it, with the context and sequence beside it
faults ≠ 0              → ALARM
else                    → IDLE
+ machineFaults
```

* A live sequence publishes by name, whatever the context: `home` and
  `recovery` as HOMING, `probe` as PROBING. A step or sequence outranks the
  job context (a jog while paused is JOGGING); a manual context keeps its
  name over its raw legs (HOMING, not TRAVERSE).
* Computed by `machinePublish()`, last in each loop pass; nothing else writes
  `machineState`. Core 0 logic never reads it: it asks the levels.
* `resumeOrHold`, `returnState`, `slotMapGate`'s alarm write and the
  re-derive checks go.

### Layers

```text
Core 1 ─facts─► observer ─► planes ─► runner ─► sequence ─► machinePublish
                    │          │         │                       │
              projections   machineAdmit  mailbox          machineState
                                                           machineFaults
Core 1 reads machineState and the ring owner: may I move, must I sweep.
```

* **Each level is written by its owner:** contexts by their modules (job,
  `ops/probe`, manual homing), the sequence by the driver, the step by the
  runner and the planner queue, faults by whoever detects them.
* **`ops/machine`** holds what crosses levels:
  * `machineAdmit(kind)`: the admission rules below;
  * `machineTeardown(estop)`: step, sequence (`abort()`), context, in that
    order;
  * `machinePublish()`;
  * `faultSet(f)` (sets, tears down), `faultsClearEvents()`.
* **Core 1** stops writing the state. It posts facts (`ESTOP_SWEPT`,
  `RING_DRAINED`) and keeps the job's at-rest reasons (BUFFERING ↔ RUNNING,
  HOLDING → PAUSED) and the estop sweep.
* Loop order: observe, planes, runner, sequence, publish.

### Command kinds

A column in the command tables; `machineAdmit` applies the kind's rule.
Argument-dependent checks stay in the handlers (`cmd/gate.h`).

| Kind | Examples | Admitted |
|---|---|---|
| read | `get`, `status`, `cfg`, `nodestat` | always; bus reads not while RING |
| write | `setorigin`, `select`, `wzero`, `axes_enable`, `bus_exclude`, `mesh`, `CFG_SET`, peripherals | not while a sequence or step is live; bus writes not while RING; peripheral on not with faults |
| step | `leg`, `jog`, `line`, `probe_leg` | not while a sequence or step is live |
| sequence | `home`, `jogto park` | same |
| context | open `probe_map`, `JOB_HDR`; close `probe_end`, `cancel`, `home_end` | manual contexts open only in IDLE |
| context control | `pause`, `resume` (`TOOL_REC(N)`), records | by the context's own rules |
| clear | `unalarm`, `unstop` | when their fault or latch is set and nothing runs |
| stop | `stop` | always; outranks everything |

Kind is separate from where a command lives. `controller/cmd/` means only
that it reads the config (`err unconfigured` without one); whether it takes
time decides step or sequence:

| | Instant (read, write) | Takes time (step, sequence) |
|---|---|---|
| **Primitive** (`cmd/`, config-free) | `setorigin`, `bus_exclude`, `axes_enable`, `get`, `stop` | `leg`, `probe_leg`, `line`, `jog` |
| **Controller** (`controller/cmd/`, reads config) | `select`, `wzero`, `wset`, `wclear`, `mesh`, `unalarm` (its instant fixes) | `home`, `jogto park`, `probe`, `unalarm`'s recovery |

* Inside a manual context only its own commands, reads and `stop`.
* LEGS may open with `F_LATCHED` set (getting off the switch is the cure).

### unalarm

After a fault the context is always IDLE (the teardown closed it), so
`unalarm` depends on the mask only:

1. Estop latched: `err estop` (`unstop` stays its own command).
2. Clear the event bits; their cleanup already ran at teardown.
3. Instant fixes in dependency order, bus then map; the first that fails
   stops it with `err <what>`.
4. `F_LATCHED` still set: start the `recovery` sequence, one LEGS step
   retracting each latched node; `ok` once started.
5. The reply carries what is left; publish shows it (`F_CONFIG` stays until
   a config).

## Branches

M1 → M2 → M3 is a dependent chain (Single Session). M3a, M4 and the
features follow M3.

## Branch M1: `feature/fault-mask`

### Plan

**Type:** feature (wire change).

**Purpose:** `machineFaults` replaces `alarmReason`; the estop latch; ALARM
and IDLE computed from faults.

* `ipc/shared_state.h`: `machineFaults`, the bits, the estop latch and
  `swept`; `AlarmReason` goes.
* `ops/machine.{h,cpp}` (new): `faultSet`, `faultsClearEvents`, the estop
  latch calls, and `machinePublish()` for the at-rest part (ALARM/IDLE from
  the mask). Core 1 still writes its states in this branch; publish leaves
  them alone.
* Writers moved: `homingFail`, `probeFail`, `slotMapGate`, `factsLost`,
  `cmdStop`, `cmdUnstop`; `resumeOrHold` goes.
* `unalarm`: steps 1–3 and 5 above (the recovery sequence waits for M3).
* Wire: `STATUS_RSP` and `get` carry the mask; web mirrors it
  (`web/src/wire/format/status.ts`), and `AlarmReason` goes there too.
* Behaviour changes: several faults show at once; `unalarm` runs every
  instant fix in one go; `cancel` ends in ALARM if a held bit is set.

**Tests (native):** publish per bit and priority; event bits survive a held
bit clearing; `unalarm` order; estop latch and swept.

**Depends on:** R1 (done).

### Status

Done (merged at `0a1d218`). Unblocks M2.

### Outcome

* Two publish calls, not one: `machinePublish()` rewrites only IDLE/ALARM
  (each pass, and after a held bit may change); `machineSettle()` replaces
  `resumeOrHold` where Core 0 closes HOMING, TRAVERSE or PROBING. Both take
  `plannerLock` (Core 1 leaves IDLE for motion under it) and never overwrite
  ESTOP.
* The estop latch is `F_ESTOP` = bit 15 of the mask, not a separate byte.
  No `swept` flag yet: Core 1 still publishes ALARM after its sweep, so
  "latched and ALARM" is swept. M2 adds the flag with Core 1's state writes.
* `STATUS_RSP` is 31 bytes, same magic: the mask at [4..5], the rest one byte
  later. `get faults=0x....` replaces `alarm=`; `unalarm` replies
  `ok faults=0x....` (web `unalarm()` resolves to the mask, null on err).
  F_BUS stops `unalarm` with `err degraded` (its fix is `bus_exclude`).
* `slotMapGate` is gone (callers publish). `F_FACTS` is set at any time and
  shows at rest.
* Native: `test_machine` (publish per bit, settle, event bits, latch) with
  stubs in `test/stubs/`; `unalarm` itself is untested natively.
* Web: the homing sequence's HOMING_FAIL diagnostics were removed;
  `web/demo/comms.js` still uses the old names.
* Bench (COM8): boot `F_BUS` → `bus_exclude 8` → IDLE; a failed `home`
  (node 3 budget, the known broken switch) → `F_HOMING`, cleared by
  `unalarm`; `stop`/`unstop` with `err estop` in between; node 1 seek →
  `F_LATCHED` under HOMING, ALARM at `home_end`, kept by `unalarm`, cleared
  by a retract.

## Branch M2: `feature/machine-slots`

### Plan

**Type:** feature.

**Purpose:** the context and step levels, `machineAdmit`, `machineTeardown`,
and Core 1 no longer writing the state.

* Contexts IDLE, JOB, PROBE, HOMING; `returnState` and the probe-from-job
  path go (a probe inside a job waits for the probe sequence, M4).
* Step level written by the runner and the planner queue; ring owner replaces
  `joggingReason`. Core 1 posts facts instead of writing JOB, JOGGING and
  IDLE (`follower.cpp:70,98,134`, `core1.cpp:106`).
* Leg owner check by step kind and generation (`ops/leg`, `ops/runner`,
  `homing.cpp`, `traverse.cpp`).
* Command kinds as a table column; gates in `cmd/gate.h` move to
  `machineAdmit`.
* `machinePublish()` takes over the whole state.
* From M1: the estop `swept` flag, set by the `ESTOP_SWEPT` fact; `unstop`
  and the latch checks test it instead of "latched and ALARM".
* `machineSettle()` goes; its callers (`homingFail`, `traverse`,
  `probeFail`/`probeSettle`, `home_end`, setorigin, boot) close their context
  or step instead.
* The "ALARM with faults == `F_LATCHED`" leg gates (`cmd/axis.cpp`,
  `legAdmitDenies`) become `machineAdmit`'s LEGS-with-`F_LATCHED` rule.

**Tests (native):** admission per kind and level; teardown order; publish
from levels; a stale drain dropped.

**Depends on:** M1.

### Status

Done (merged at `69a37a1`). Unblocks M3.

### Outcome

* Deviations:
  * The two Plan commits are one: once Core 1 stops writing the state, Core 0
    must publish it in the same change.
  * The step is derived, not stored: `machineStep()` reads the runner, the
    probe leg and the ring (`plannerActive`, count). So there is no stale
    drain to drop, and that test is gone.
  * The leg owner is a context (`legOwnerHolds`: no estop requested and the
    context unchanged), not a step kind and generation. Traverse legs run
    under CTX_IDLE.
  * Core 1 sets `estopSwept` itself before pushing `ESTOP_SWEPT`.
    `estopRequested` (Core 0's `stop`) replaces Core 1's reads of ESTOP.
  * The data plane has no kind column; its gates and `configStoreCommit`
    read the levels.
* Interfaces M3 relies on (`ops/machine.h`):
  * contexts and the ring owner: `machineContext` and `ringOwner` (values as
    `joggingReason`) in `ipc/shared_state.h`; `contextOpen`/`contextClose`;
  * the step and teardown: `machineStep`, `machineTeardown(estop)`
    (the step first, then the context), `jobAbort`;
  * admission: `machineAdmit(kind)` with `CmdKind` and the
    `K_IN_PROBE`/`K_IN_HOMING`/`K_BARE_READ` flags on each `cmd/table.h` row;
    `machineView` (the state from the levels) and `machineAtRest`/
    `machineBusFree` for argument checks.
  * `homingHeld()` stands in for the sequence level (`sequenceLive` in
    `machine.cpp`); M3 replaces it.
* Behaviour:
  * A live step or `home` run refuses as `err busy` (some were `bad_state`).
  * `F_FACTS` tears down what runs.
  * `probe_map` opens from IDLE only. An estop in a probe ends it with no bus
    traffic.
  * Peripherals refuse `on` with any fault. `status` names PROBING.
  * A paused job's parked ring is not a live step. Found on the bench, where
    every write in a paused job answered `busy`.
* Bench (agent, COM8, `controller-1head.jsonc`, node 8 excluded):
  * homing: all four axes homed, with `busy` refusals during the run;
  * estop: `stop` during homing, a job and a dummy leg each closed the
    context, and `unstop` returned to IDLE;
  * dummy legs: a failing one gave `F_HOMING`, with `home` refused until
    `unalarm`;
  * jogs: X, Y, Z, A and `jogto` showed JOGGING, then IDLE;
  * a job: refusals while running; pause with writes and `nodepos` allowed,
    `jog`/`home` as `bad_state`; resume; `cancel` once dry; the abort byte
    braked to IDLE with homes kept.
* Not tested (deferred by the user): probing (`probe_map`, probe legs,
  `probe_end`); the continuous jog packet; a leg failing mid-move.
* JB1's open human check "probing from a paused job" no longer applies:
  probing opens from IDLE only until M4.
* Checks: `pio run -e pico` clean; `pio test -e native` 105/105.
* Out of scope: `docs/tool_probe.md` still describes `returnState`, probing
  from PAUSED and `busGateDenies`. `vac_pump on|off` answered `err usage` on
  the bench, so the right syntax is unknown and peripherals went untested.

## Branch M3: `feature/sequence-driver`

### Plan

**Type:** feature.

**Purpose:** the sequence level, `next()`/`abort()`, step builders, WAIT
steps, and `home` on the driver.

* `controller/seq/sequence.{h,cpp}` (new); `controller/seq/home.cpp` rebuilt
  on it (cycles, phases, `onSwitch`, sweep reads and datum commits in
  `next()`); `sequenceLive()` in `ops/machine.cpp` (today `homingHeld()`)
  reads the sequence level.
* `home` runs as a sequence in IDLE with no context: split the shared leg
  arming from the manual HOMING session (`homingBegin`/`homingParkBegin`
  open CTX_HOMING today, and `homeTick` ends on its close). Raw `leg` keeps
  the session; latch and origin effects stay shared (`leg_effects`).
* Every step belongs to a sequence. A raw `leg` is a one-step `leg`
  sequence (in the manual HOMING context), and a traverse leg a one-step
  `traverse` sequence (in IDLE); their `next()` replaces `homingTick` and
  `traverseTick`. Raw legs run one at a time: a `leg` while any sequence is
  live is refused as `busy` (parallel raw legs go). A sequence like `home`
  still arms several legs per step itself.
* Ownership by the chain: the runner's mailbox has one reader, the live
  sequence. `legOwnerHolds`, the runner's owner (and the owner argument to
  `runnerArm`/`runnerPop`) and its generation go (`boxClear` on a cut already
  drops stale ends). `contextClose` aborts a live sequence and step;
  `sequenceAbort` cuts the step; `machineTeardown` goes step, sequence,
  context. `home_end` refuses as `busy` while a leg runs, as `setorigin`
  does.
* `homingFail` (abort the other legs, `failWhy`/`failNode`, `F_HOMING`) is
  shared by `home`, `leg` and `traverse`; the fail data stays in
  `ops/homing`.
* WAIT comes from the sequence: `machineStep()` returns it while the live
  sequence waits. A wait has a timeout and an event from a fixed enum, each
  value tied to the command that posts it; `machineAdmit` admits that
  command while the wait is live. M3's only event is the timeout (JB2 adds
  `mount`).
* Publish: a sequence by name; step and sequence before the job context.
* The `recovery` sequence for `F_LATCHED` (`unalarm` step 4): step 1 takes
  every latched node off its switch in parallel, `LEG_UNLATCH` where the
  origin is valid (a span latched it; the origin stays) and `LEG_RETRACT`
  where not (a seek latched it); step 2 parks the nodes that kept their
  origin at their park position.
* Not here: the `SEQ` ring owner (J1b, `jogto park`); admission in
  `JOB.PAUSED` (JB3, M4).

**Tests (native):** a run with legs, ring and wait; a failing leg; `stop`
mid-step; a wait timing out; `abort()` on teardown.

**Checks:** human: `home` on the machine, a failing switch.

**Depends on:** M2.

### Status

Done (merged at `288a6bd`). Unblocks M4.

### Outcome

* Deviations:
  * The driver is `ops/sequence.{h,cpp}`, not `controller/seq/`: it reads no
    config, and `ops/machine`, `ops/homing` and `ops/traverse` call it.
    `home` stays in `controller/seq/`.
  * `recovery` lives in `controller/seq/home.cpp`, sharing `home`'s leg
    numbers (`intervalUs`, `distSteps`, `parkSteps`). Its legs are `home`'s
    back-off leg: away from the switch at `latchFeed`, `backoffDist` as the
    budget. A latched node that is not a homeable linear axis refuses
    `unalarm` with `err node N not_homeable`.
  * A wait's posting command is not admitted yet: M3 has none (the timeout is
    its only event). JB2 adds it with `mount`.
  * No test runs a sequence through a RING step: the `SEQ` ring owner is J1b's.
  * `leg_abort` fails a `leg`, `home` or `recovery` sequence, as before for
    the homing context; on a traverse leg it only ends the step.
* Interfaces M4 relies on:
  * `ops/sequence.h`: `Sequence { id, next(end), abort }`, `sequenceStart`,
    `sequenceEnd` (a refused start), `sequenceAbort`, `sequenceWait(ms)`,
    `sequenceLive`/`sequenceId`/`sequenceWaiting`; M4 adds its id to
    `SequenceId` (and PROBING to publish).
  * `ops/homing.h`: `homingArm`, `homingArmPark`, `homingArmUnlatch` arm into
    the live step with no context; `homingFail`/`homingAbandon` fail the live
    sequence.
  * The runner: no owner argument; `runnerPop(ev)`.
  * `machineStep()` returns `STEP_WAIT`; publish names `home`/`recovery`
    HOMING in any context.
* Behaviour:
  * A step command is `busy` while any step or sequence is live: one raw leg
    and one probe leg at a time.
  * `home_end` is `busy` while a leg runs.
  * `unalarm` with `F_LATCHED` starts `recovery`, `ok faults=0x0008` once it
    runs (the latch clears as it does).
* Fixed in passing: `legTimeoutMs` timed every ramp step at
  (start + floor) / 2, so a park shorter than its two ramps (recovery's are)
  could hit `LEGFAIL_DEADLINE` 2 steps short. Each end now ramps over its share
  of the move.
* Bench (agent, COM8, `controller-1head.jsonc`, node 8 excluded):
  * `home`: HOMING while running, `busy` for `home` and `leg`, IDLE with all
    four homed;
  * manual session: a second leg and `home_end` `busy` during a dummy leg,
    `homing=1` between legs, `home_end` to IDLE; a failing dummy gave
    `homefail=7`;
  * traverse: a park published TRAVERSE, with `leg`, `dummy_leg` and `home`
    `busy`; `leg_abort` ended it with no fault (the aborted park drops its
    origin, as before);
  * `recovery`: retract (seek latched, no origin) to IDLE; unlatch and park
    (span latched, origin kept) 5/5 after the deadline fix (1 of 3 failed
    before it);
  * the deadline: a scratch build with a 0.6 margin fired it (`homefail=3`);
    the real build ended the same leg on its budget (`homefail=1`).
* Not tested: a real failing switch during `home` (only dummy failures);
  `recovery` with several latched nodes at once.
* Checks: `pio run -e pico` clean; `pio test -e native` 112/112.

## Branch M3a: `feature/homing-threads`

### Plan

**Type:** feature.

**Purpose:** per-node homing. Each node of a cycle runs its own chain of legs
(seek, back-off, latch, pull-off) instead of all nodes advancing a phase in
lockstep, so a cycle takes its slowest node's chain rather than the sum of
each phase's slowest leg. The cycle stays the barrier: `cycleCommit` is the
join.

* Every arm (`runnerArm*`, `homingArm*`) takes an explicit `last` flag, with
  no default: true on a leg that is not last commits a cycle mid-move, so
  each caller states it. The runner keeps a
  thread-open bit per node: arming opens it, a `last` leg's end closes it, and
  the step ends only when no leg is live and no thread is open. Without it, a
  node that arms its next leg on a leg end would follow a step end already
  posted for the moment nothing ran, and the cycle would commit while it
  moves.
* The sequence gains a per-leg hook, so a node arms its next leg on its own
  ends; home keeps `phase` per `RunNode`, and `sweepsRead` runs per rotary
  node.
* Home knows each node's last leg at arm time (linear pull-off, rotary
  reverse sweep, park). A leg released through `runnerAbort` must close its
  thread.
* Every other sequence passes `last` true and runs in lockstep as today;
  probe, whose legs end on their result, too.
* Tests: nodes ending out of order, the join firing only on the last thread,
  a failure while other threads run. Bench with axes started at uneven
  distances from their switches.

**Depends on:** M3.

### Status

Done (merged at `34db55c`). Unblocks nothing; M4 and J1b build on its
threads and `leg` hook.

### Outcome

* Interfaces later branches use:
  * `last` comes right after the node: `runnerArm(intent, node, last, …)`,
    `runnerArmPark(node, last, …)`, `runnerArmDummy(last, …)`, and the same
    in `homingArm`, `homingArmPark`, `homingArmUnlatch`.
  * `Sequence` gains a last field, `leg(const LegEnd&)`, may be null: called
    for each leg that succeeds. Sequences without it are unchanged.
* Home: `phase` and `done` per `RunNode`; `homeLeg` arms a node's next leg
  (reading a rotary node's sweep first); `homeNext` is the cycle join only.
  `armPhase`, `armFrom` and `sweepsRead` are gone.
* Also here: the plan's Slots became Levels (stream slots kept the name),
  in this doc and in comments of `machine.h`, `sequence.h`, `test_machine`.
* Bench (COM8, `home` from a homed machine, 3 runs each):

  | | cycle 1 (Z) | cycle 2 (X, Y, A) |
  |---|---|---|
  | `main`, lockstep | 21–24 s | 37.6–37.9 s |
  | M3a, per node | 22–24 s | 28.3–29.0 s |

  Regression: dummy legs (ok, and F_HOMING then `unalarm`), a span with the
  origin kept, a span into the switch latching X, and recovery clearing it,
  all as on `main`.
* Not tested: a per-leg trace (`nodestat` is refused while homing, so the
  evidence is the timing); axes started at deliberately uneven distances; a
  real failure in one thread while others run (unit-tested only); no
  `test_home` suite, as agreed.
* Out of scope: recovery could run its unlatch and park per node the same
  way.
* Checks: `pio run -e pico` clean; `pio test -e native` 119/119.

## Branch M3b: `refactor/cjog-ops`

### Plan

**Type:** refactor.

**Purpose:** the continuous jog's behaviour moves out of the data plane into
`core0/ops/cjog.cpp`, so it can be tested natively. Same behaviour.

* Moves from `core0/data_plane.cpp`: `cjogStart` (room and feed), the
  replace-or-hold branch of `acceptCjog`, `cjogTick` (deadman, end of
  travel, held start), `cjogStop`, and the cjog state.
* Interface: `cjogPacket(d, speed, now)` and `cjogTick(now)` return the reply
  (none, ACK, or a NACK code); `cjogStop()`. No `millis` or `Serial` in the
  module; the data plane parses, validates the packet, and writes replies.
* `test/test_cjog`, added to `test_filter` (`platformio.ini`), faking the
  planner queue, the config and frames lookups, and `machineAdmit`: deadman,
  repeats, direction change and held start, room and feed, `SOFT_LIMIT`,
  unhomed.
* Read first: whether `config/machine_cfg.h` and `ops/frames.h` compile
  natively.

**Depends on:** nothing.

### Status

Done (merged at `03116a3`). Unblocks M3c.

### Outcome

* As planned, plus: the packet checks (direction range, speed 0, mixed
  axes) moved into `cjogPacket` too, so they are tested; the data plane
  keeps the CRC and framing. Replies are `CJOG_NO_REPLY`, `CJOG_ACK`, or
  an `MSEG_NACK_*` reason; `cjogReply` writes them.
* The held start's check is `machineView()` (no `machineAdmit` yet); the
  tests fake it. M3c swaps it.
* `configAxisRange` is faked rather than linked; no other native trouble.
* For M3c (found here, both on `main`):
  * A direction change holds the new direction; a packet for it that
    arrives at rest before the tick starts it directly and leaves it held,
    so at the end of travel the tick starts it again and NACKs
    `SOFT_LIMIT`. Fix: `cjogStart` clears `cjogWant` when it queues; a test
    reproduces it.
  * `test_sequence.cpp:58` warns: the `scripted` sequence lacks its `leg`
    (`nullptr`).
* Checks: `pio run -e pico` clean; `pio test -e native` 137/137
  (`test_cjog` 18).

## Branch M3c: `fix/dataplane-admit`

### Plan

**Type:** fix.

**Purpose:** a continuous jog admits through the levels, as the command
tables do. Today it checks nothing (`core0/data_plane.cpp:338`): during a
leg step the ring is empty and idle, so `plannerJogFrom` accepts and Core 1
emits the jog while legs hold the bus (`home`, `leg`, recovery, probe legs).

* A jog's own ring (`cjogOn`, or `RING_JOG_STEP`): stopped and held, no
  admission; every start, direct or held, passes `machineAdmit(K_STEP)`.
  The own-ring test only limits the stop to jogs; no new command kind.
  `busy` → `MSEG_NACK_BUSY`, else `BAD_STATE`. The held start's `machineView() != STATE_IDLE` goes, so it
  also starts in ALARM with event faults, as `jog` does.
* `MSEG_NACK_BUSY` (0x0B, `core0/usb_protocol.h`), mirrored in
  `web/src/wire/` (constant, name table, export) and the sim (`busy` for
  TRAVERSE and HOMING).
* Test in `test_cjog`, failing before the fix: with admission answering
  `busy`, a packet returns `BUSY` and queues nothing.
* Records keep their label check (`data_plane.cpp:134`): correct today, and
  a levels version would rebuild most of `stateOf`.
* Docs: `docs/wire_protocol.md` (also the missing `0x07 ABORTING` row).
* Human checks: a cjog during `home` and during a raw `leg` is NACKed
  `busy` and the legs run on; a cjog direction change still restarts.

**Depends on:** M3b.

### Status

Planned.

## Branch M4: `feature/probe-sequence`

### Plan

**Type:** feature.

**Purpose:** `probe` as a sequence (bind, park Zs, XY to the switch, probe
legs, read, restore), usable inside a job; a probe profile in the config.

* Admission in `JOB.PAUSED` for the probe sequence and `home`, once JB3
  drops the ring on pause. A failure there keeps the job and its index.
* An estop during the probe sequence: `abort()` drops the binding without
  the bus, as M2's manual probe teardown does; confirm here.

**Depends on:** M3.

### Status

Planned.

## Features

| Branch | On this plan | Depends on |
|---|---|---|
| J1b `feature/jogto-named` (motion-sessions.md) | sequence: `parkZs()`, `lineTo()` | M3, M3c |
| JB2 `feature/tool-mount` | `mount` posts the event a WAIT takes | nothing |
| JB3 `feature/layer-header` | pause drops the ring; jogs admitted in `JOB.PAUSED` | JB2 |
| JB4 `feature/tool-change` | sequence in JOB.TOOL_CHANGE: park, per head XY, WAIT `mount`, later probe | M3, JB3 (M4 for probing) |

## Open

* Peripheral off under LEGS: allowed (Core 1 serves requests between leg
  polls) or refused as today.
* Whether the job's at-rest reasons also move to Core 0 later.
