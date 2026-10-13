# Machine model: faults, slots, sequences

`machineState` and `alarmReason` hold three things in two bytes: what runs,
what is wrong, and what everyone else is told. One byte cannot hold a probe
inside a paused job (`returnState`), nor two faults at once, so the code
re-derives the state from guesses (`resumeOrHold`, the "if in this alarm"
checks in `cmd/axis.cpp:231`, `ops/slot_map.cpp:179`).

This plan splits them: a fault mask, three slots for what runs, and a
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

### Slots

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
* A leg's owner check is the step slot (LEGS) and the step's generation, not
  `machineState`.

Examples:

```text
leg park 3                 IDLE         / —       / LEGS
jog x 10                   IDLE         / —       / RING JOG_STEP
jogto park, step 1         IDLE         / jogto   / LEGS
jogto park, step 2         IDLE         / jogto   / RING SEQ
the job cutting            JOB.RUNNING  / —       / RING JOB
jog while paused           JOB.PAUSED   / —       / RING JOG_STEP
probe while paused         JOB.PAUSED   / probe   / LEGS
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

* Computed by `machinePublish()`, last in each loop pass; nothing else writes
  `machineState`. Core 0 logic never reads it: it asks the slots.
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

* **Each slot is written by its owner:** contexts by their modules (job,
  `ops/probe`, manual homing), the sequence by the driver, the step by the
  runner and the planner queue, faults by whoever detects them.
* **`ops/machine`** holds what crosses slots:
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

M1 → M2 → M3 is a dependent chain (Single Session). M4 and the features
follow M3.

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

**Purpose:** the context and step slots, `machineAdmit`, `machineTeardown`,
and Core 1 no longer writing the state.

* Contexts IDLE, JOB, PROBE, HOMING; `returnState` and the probe-from-job
  path go (a probe inside a job waits for the probe sequence, M4).
* Step slot written by the runner and the planner queue; ring owner replaces
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

**Tests (native):** admission per kind and slot; teardown order; publish
from slots; a stale drain dropped.

**Depends on:** M1.

### Status

Ready to merge.

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
    read the slots.
* Interfaces M3 relies on (`ops/machine.h`):
  * contexts and the ring owner: `machineContext` and `ringOwner` (values as
    `joggingReason`) in `ipc/shared_state.h`; `contextOpen`/`contextClose`;
  * the step and teardown: `machineStep`, `machineTeardown(estop)`
    (the step first, then the context), `jobAbort`;
  * admission: `machineAdmit(kind)` with `CmdKind` and the
    `K_IN_PROBE`/`K_IN_HOMING`/`K_BARE_READ` flags on each `cmd/table.h` row;
    `machineView` (the state from the slots) and `machineAtRest`/
    `machineBusFree` for argument checks.
  * `homingHeld()` stands in for the sequence slot (`sequenceLive` in
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

**Purpose:** the sequence slot, `next()`/`abort()`, step builders, WAIT
steps, and `home` on the driver.

* `controller/seq/sequence.{h,cpp}` (new); `controller/seq/home.cpp` rebuilt
  on it (cycles, phases, `onSwitch`, sweep reads and datum commits in
  `next()`); `homingHold` becomes "a sequence is live".
* The `recovery` sequence for `F_LATCHED` (`unalarm` step 4).

**Tests (native):** a run with legs, ring and wait; a failing leg; `stop`
mid-step; a wait timing out; `abort()` on teardown.

**Checks:** human: `home` on the machine, a failing switch.

**Depends on:** M2.

### Status

Planned.

## Branch M4: `feature/probe-sequence`

### Plan

**Type:** feature.

**Purpose:** `probe` as a sequence (bind, park Zs, XY to the switch, probe
legs, read, restore), usable inside a job; a probe profile in the config.

**Depends on:** M3.

### Status

Planned.

## Features

| Branch | On this plan | Depends on |
|---|---|---|
| J1b `feature/jogto-named` | sequence: `parkZs()`, `lineTo()` | M3 |
| JB2 `feature/tool-mount` | `mount` posts the event a WAIT takes | nothing |
| JB3 `feature/layer-header` | unchanged | JB2 |
| JB4 `feature/tool-change` | sequence in JOB.TOOL_CHANGE: park, per head XY, WAIT `mount`, later probe | M3, JB3 (M4 for probing) |

## Open

* Wait events: a fixed enum or an id carried by the event.
* An estop during a probe sequence: `abort()` drops the binding without the
  bus (`unstop` forgets the map anyway); confirm in M3's Read.
* Peripheral off under LEGS: allowed (Core 1 serves requests between leg
  polls) or refused as today.
* Whether the job's at-rest reasons also move to Core 0 later.
