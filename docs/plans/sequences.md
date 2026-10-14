# Sequences: one state writer, Core 1 facts, steps that move

**Superseded from R2a on by docs/plans/machine-model.md.** R1 is done and
kept; R2a–R4 and the Decisions on session, admission and sequences are
replaced there.

Machine state is written today from many places: `ops/` (`homing.cpp:37,58`,
`probe.cpp:147,193,242`, `slot_map.cpp:177`, `state.cpp:21`,
`traverse.cpp:18`), `cmd/lifecycle.cpp:21,115`, `core0.cpp:121`, and Core 1
(`core1.cpp:63,87,104`, `follower.cpp:69,97,133`). Each operation ends by
writing the state itself, and Core 0 learns what Core 1 did by polling state
edges (`reconcileValidity`, `ops/position.cpp:357`).

Features that chain operations (J1b `jogto park`, JB4 tool change) need a
multi-step run under one state: park legs, then a planner move, then a wait.
Written per feature, each would be its own state machine polling its own
completion, and each would show the gaps between its parts (TRAVERSE → IDLE →
JOGGING).

This plan makes the session the only Core 0 state writer, turns Core 1's
signals into facts on a queue, and grows the leg runner's steps into ring moves
and waits run by a generic sequence driver. It takes over the Later items of
docs/plans/leg-runner.md it covers, and supersedes the "only ops write state"
rule of docs/plans/core0-layers.md.

## Decisions

### Layers

```text
Core 1 ──facts──► [C1 ring] ──► observer ──► runner ──► [mailbox] ──► sequence
  │                               │            ▲  │                      │
  │                               ▼            │  ▼                      ▼
  │                         projections     legs, ring moves,       session ◄── planes
  ▼                                         waits                   (admit, own, close)
machineState ◄──────────────────────────────────────────────────── the only Core 0 writer
```

* **Planes** (`cmd/`, `controller/cmd/`, the data plane): parse, gate on their
  own arguments, ask the session, reply.
* **Session** (`ops/session`): admission by state, the only Core 0 writer of
  `machineState`/`alarmReason`, opens and closes sessions, consumes events no
  sequence owns.
* **Sequences** (`controller/seq/`): config-aware lists of steps run under one
  owner state. Started by a plane (`jogto park`) or by the session itself
  (a job's tool change).
* **Runner** (`ops/runner`): runs one step at a time, applies physical
  effects, posts results. Policy-free.
* **Mechanisms** (`ops/leg`, the planner queue, the bus): do the work, never
  write state.
* Loop order: observe, planes, runner, session, sequences. A Core 1 fact is
  popped, translated, posted and consumed in the same pass.
* A primitive run standalone is a one-step sequence: the operation never
  branches on who called it; only the consumer of its end differs (the
  session's default closer, or a sequence).

### Who may write state

Replaces docs/plans/core0-layers.md's "only ops write the state machine; the
controller changes state only by calling ops".

* **Primitives** (`cmd/`): atomic and config-free, as before. They ask the
  session to run one step.
* **Sequences** (`controller/seq/`): read the config; ask the session to run
  their steps in order.
* **Session:** decides entry and exit, and each step's end: a sequence takes
  it, or the owner's default closer closes the state.
* **Ops:** do the work and post how it ended. They never write state.
* **Core 1:** estop and alarm stop, and reasons inside the state the session
  opened (see below).

Rejected: a mode on each primitive, writing state when standalone and
returning a result inside a sequence. Every op would carry two endings that
drift apart; with one-step sequences the op has one ending and only its
consumer differs.

### Core 1 writes and facts

* **Core 1 keeps writing** what must be true before anyone can observe it:
  * estop and alarm stop, with the sweep before ALARM is published, so ALARM
    visible implies the bus is parked;
  * reasons inside a session decided at rest under `plannerLock`
    (BUFFERING ↔ RUNNING, HOLDING → PAUSED, resume at rest);
  * motion entry from IDLE (`follower.cpp:93-105`).
* **Core 1 posts facts** for what Core 0 decides: `ESTOP_SWEPT`,
  `RING_DRAINED` (at rest, ring empty). Facts report; they never cause. An
  alarm is in effect before its fact is read.
* **Session exits move to Core 0:** JOGGING and JOB leave to IDLE through the
  session on `RING_DRAINED`, closed under `plannerLock` with the ring checked
  empty again.
* **The observer** pops facts (exactly once; no edge latches, no paired
  state/reason reads) and keeps the projections (`axes_enabled` from
  `nodeEnabled`).

### Queues

| | Core 1 ring | Runner mailbox |
|---|---|---|
| Crosses cores | yes: SPSC, barriers on publish and consume | no |
| Producer | Core 1 step loop | runner, on Core 0's tick |
| Consumer | observer only | the step's owner: sequence or session |
| Content | facts | leg ends, step ends |
| Full | the fact is dropped and `ALARM_FACT_RING_FULL` raised; the observer reads the level state once and fences only if it shows an estop | cannot happen: at most one leg per node plus the step end |

* Every mailbox event carries the generation of its step. Opening, aborting or
  dropping a step bumps it; an event of an older generation is dropped when
  popped. This is what keeps a late bus reply from an aborted leg from being
  taken as a new step's leg ending.

### Steps

* A step is one kind:
  * **legs:** one or more node legs in parallel, one per node (as today);
  * **ring move:** blocks pushed to the planner; done on `RING_DRAINED`;
  * **wait:** done on a named event (`mount`, …) or a timeout.
* Legs and the planner ring never move at the same time.
* A step posts one `RUN_STEP_END`, after its last part. A legs step also posts
  each `RUN_LEG_END` as it happens.
* A step's owner judges each end (`legEndFailed`, as `homingTick` does). The
  default policy: any failure aborts the step (`runnerAbortAll`), drops the
  sequence's remaining steps, and fails the session with the owner's alarm.
* A refusal after the `ok`, arming a step or in instant work, fails the
  sequence with `sequenceFail(why)`: the SEQUENCE fault, `seqfail=<why>`.
  Refusals before the sequence starts are the command's reply.
* `stop` aborts the live step of any kind, drops the sequence, and ends as
  `stop` does today.

### Sequences

* A fixed list of steps, built when the sequence starts, with one owner state
  for the whole run. No branching beyond fail → abort; loops (per head) are
  written out when the list is built.
* The owner state's reason byte says which part runs (`JOGGING_GOTO`,
  `JOB.TOOL_CHANGE`); the state does not change between steps.
* The session closes the state when the sequence ends: IDLE through
  `resumeOrHold`, or the job's next reason.

### Admission

* A standalone primitive that takes time (a leg, `home`, `probe`, a plain
  `jogto`) is a one-step sequence; one sequence runs at a time.
* Instant primitives (`setorigin`, `bus_exclude`, `axes_enable`) run inline
  in the plane once admitted: no step, no generation.
* A command is admitted in two checks:
  1. the state gate: allowed in this owner state at all (`cmd/gate.h`, as
     today);
  2. the session: while a sequence is live, a command that opens a step or
     changes what a step relies on (`axes_enable off`, `setorigin`,
     `bus_exclude`, `CFG_SET`) gets `err busy`.
* Always taken: `stop` and estop (abort the live step, drop the sequence), and
  read-only commands (`get`, `status`, `cfg`, bus queries).
* A sequence is live between its steps too, and during a wait only the
  awaited event and `stop` get through.
* A job's records and `pause`/`resume`/`cancel` keep their gates; `err busy`
  applies in a job only while a sequence (a tool change) runs in it.

### Event mocks

`jogto park`, two heads (Z nodes 3 and 5), one row per Core 0 loop pass.

```text
pass  input / tick                C1 ring         mailbox                    consumer acts
 1    plane: `jogto park`         —               —                          session admits: JOGGING.GOTO
                                                                             seq step 1 = park Z3, Z5 → gen 7
 3    Z5 at target                —               LEG_END g7 Z5 ok           seq: wait
 4    Z3 at target                —               LEG_END g7 Z3 ok           seq: wait
                                                  STEP_END g7                seq step 2 = ring move → gen 8
41    Core 1: ring dry, at rest   RING_DRAINED    —                          observer → runner: step 8 done
                                                  STEP_END g8                seq done → session: IDLE
```

A park fails:

```text
 3    Z5 stopped on a switch      —               LEG_END g7 Z5 FAIL         seq: abort step (Z3 aborted inline,
                                                                             mailbox cleared, gen → 8);
                                                                             session: ALARM_HOMING_FAIL, LEGFAIL_PARK
 4    late reply from Z3          —               LEG_END g7 Z3 aborted      g7 ≠ 8 → dropped
```

`stop` mid-park, then a new `jogto`:

```text
 2    plane: `stop`               —               —                          session: abort step, drop seq, gen → 8
 3    Core 1 sweep done           ESTOP_SWEPT     —                          observer: invalidate, fence
 4    late reply from Z5          —               LEG_END g7 Z5 ok           g7 ≠ 8 → dropped
60    plane: `jogto park`         —               —                          seq step 1 → gen 9
61    late reply from Z3          —               LEG_END g7 Z3 ok           g7 ≠ 9 → dropped
```

A standalone park leg on node 3: a one-step sequence, ended by the session's
default closer.

```text
 1    plane: park leg, node 3     —               —                          session admits: TRAVERSE
                                                                             one step = leg Z3 → gen 12
 4    Z3 at target                —               LEG_END g12 Z3 ok          session: wait
                                                  STEP_END g12               session: resumeOrHold → IDLE
```

The same leg as step 1 of `jogto park` posts the same events; only the
consumer differs (the sequence, which opens step 2).

## Branches

R1 → R2a → R2b → R3 is a dependent chain (Single Session). R4 is optional.
Features follow in docs/plans/motion-sessions.md.

## Branch R1: `refactor/core1-events`

### Plan

**Type:** refactor. Behaviour unchanged.

**Purpose:** the Core 1 → Core 0 fact ring, and the observer as its consumer.

* New `src/rp2350/ipc/core1_facts.{h,cpp}`: an SPSC ring of one-byte facts
  (`FACT_ESTOP_SWEPT`, `FACT_RING_DRAINED`), 8 slots. `core1FactPush` (Core 1)
  returns false when full and sets the sticky `core1FactsLost`, written only by
  Core 1 and cleared by the observer; `core1FactPop` (Core 0) returns
  `FACT_NONE` when empty. No payload: a byte slot can't tear.
* Core 1 posts `ESTOP_SWEPT` after the sweep publishes ALARM (`core1.cpp:87`),
  and `RING_DRAINED` where it writes IDLE today (`follower.cpp:133`
  `leaveIdle`, `core1.cpp:104` abort at rest). `leaveDry` posts nothing: that
  job is buffering, not done. Core 1 still writes IDLE in R1; `RING_DRAINED`
  is popped and ignored until R2b.
* `RING_DRAINED` carries no generation (Core 1 doesn't know Core 0's steps).
  Its consumer re-checks under `plannerLock`, ring empty and `!plannerActive`;
  a stale drain fails the check and is dropped.
* `ops/position.cpp:357` `reconcileValidity` is split up:
  * Origin invalidation moves to the estop writer, `cmd/lifecycle.cpp:21`:
    origins die when the estop starts, before the sweep, as today.
  * The fence (`:389-397`) runs on `ESTOP_SWEPT`.
  * The `axes_enabled` projection (`:411-416`) stays in `position.cpp` as
    `projectEnabled()`.
  * Both edge latches go.
* New `ops/observe.{h,cpp}`: `observeTick()` pops facts, handles overflow, and
  calls `projectEnabled()`; `core0.cpp:168` calls it. Overflow: read the level
  state once (fence if `alarmReason == ALARM_ESTOP`), and raise
  `ALARM_FACT_RING_FULL` unless already in ALARM.
* `ALARM_SOFT_LIMIT` has no writer today (soft limits are refused at
  admission); its arm in the edge code goes, and the reason stays in the enum.
* `ALARM_FACT_RING_FULL`: a new `AlarmReason`, mirrored in
  `web/src/wire/format/status.ts`.

**Tests (native):** ring order, wrap, overflow flag, empty pop.

**Checks:** `pio run -e pico`, `pio test -e native`, `pnpm typecheck`/`test`
for the reason. Human: an estop still invalidates datums and fences slots;
the ESTOP recovery flow (`axes_enable on` → `setorigin` → `unalarm`) is not
stomped.

**Depends on:** nothing.

### Status

Done (merged, d9591d4..54aa1dc). Unblocks R2a.

### Outcome

* Deviations:
  * The estop's origin invalidation and fence run in `cmdStop`, right after
    the ESTOP write (`slotFenceAllBound`); only the fenced slots' `machinePos`
    waits for `ESTOP_SWEPT` (`slotFencedPosClear`), since Core 1 writes it
    until the sweep. Today's fence raced Core 1's last slot.
  * Every `stop` invalidates and fences, including a second one during
    recovery, which wipes a `setorigin` taken before `unstop`.
  * `ALARM_FACT_RING_FULL` is raised only from IDLE, under `plannerLock`; in
    ALARM the flag is dropped and the reason kept; elsewhere it waits for
    IDLE.
* Interfaces later branches rely on: `core1FactPush/Pop`,
  `core1FactsLost/LostClear/Reset` (`ipc/core1_facts.h`); `observeTick()`
  (`ops/observe.h`), first in the loop; `RING_DRAINED` is popped and ignored,
  for R2b to consume.
* Bench (COM8): `stop` mid-jog → `homed=0` at the `ok`, ALARM(ESTOP) with
  `pos=0` 70 ms later; `slot_map !1 !2 !3 !4`; second `stop`; `unstop` → IDLE.
  Overflow is covered by `test_ipc` only.
* Out of scope: `web/test` still has 13 files failing to load and 1 failing
  test, as on `main`; `pnpm typecheck` has the same 40 errors as `main`.

## Branch R2a: `refactor/session`

### Plan

**Type:** refactor. Behaviour unchanged.

**Purpose:** `ops/session` as the only Core 0 writer of `machineState` and
`alarmReason`.

* `ops/session.{h,cpp}` (new), with a default closer per owner:

  | Call | Does |
  |---|---|
  | `sessionOpen(owner, reason)` | admits by state, opens; returns the refusal (`bad_state`, `busy`) |
  | `sessionClose()` | ends the open session, then settles |
  | `sessionSettle()` | at rest only: re-evaluates the held reasons after a condition changed; a no-op while a session is open |
  | `sessionAlarm(reason)` | an event alarm, from any state but ESTOP; ends an open session first |
  | `sessionEstop()` | ESTOP, then `originInvalidateAll()` and `slotFenceAllBound()` (R1's order in `cmdStop`) |
  | `sessionReset()` | the soft-reset wipe: drops any session, ALARM/NODE_FAULT and the sub-reasons |

* `resumeOrHold` (`ops/state.cpp:11`) becomes the session's private
  `settle()`. Held reasons (bus degraded, config, slot map, limit latched) are
  standing conditions and clear through settle; event reasons
  (`sessionAlarm`) clear only through `unalarm`, which clears the reason and
  calls `sessionSettle()`.
* Move the writers onto it:
  * `sessionOpen`: `homing.cpp:58`, `probe.cpp:240`, `traverse.cpp:18`.
  * `sessionClose`: `home.cpp:300`, `axis.cpp:350`, `probe.cpp:145,147`,
    `traverse.cpp:48`, `cmd/lifecycle.cpp:57,120,137`.
  * `sessionSettle`: `axis.cpp:231,360`, `core0.cpp:162`, `slot_map.cpp:179`,
    `controller/seq/controller.cpp:14`, `controller/cmd/unalarm.cpp:31`.
  * `sessionAlarm`: `homing.cpp:37`, `probe.cpp:191`, `slot_map.cpp:177`,
    `ops/observe.cpp:21-23` (`ALARM_FACT_RING_FULL`).
  * `sessionEstop`: `cmd/lifecycle.cpp:24` and the invalidation and fence
    after it.
  * `sessionReset`: `core0.cpp:124-128`.
  Line numbers as of `7a72e85`; Read confirms each call's kind.
* Admission by state moves into `sessionOpen`; checks that depend on arguments
  stay in the handlers (`cmd/gate.h` says why). Today's refusals stay;
  `err busy` (see Admission) is a wire change and comes with J1b, the first
  command that can meet a live sequence, mirrored in the web host.
* `traverseTick`'s close becomes the TRAVERSE default closer.
* A seam (state read/write, `plannerLock`) so the session compiles in
  `native`.

**Tests (native):** open/close per owner, refusal per state, alarm from IDLE
and from an open session, settle per held reason, estop, reset, the default
closers.

**Checks:** as R1. Human: home, probe, `leg park`, a job with pause and
cancel, estop and `unstop`: as before.

**Depends on:** R1.

### Status

Planned.

## Branch R2b: `refactor/session-exits`

### Plan

**Type:** refactor. One timing change: IDLE after a drained ring is published
by Core 0, one loop pass after Core 1 is at rest.

**Purpose:** JOGGING and JOB leave to IDLE through the session on
`RING_DRAINED`; Core 1 stops writing IDLE.

* `follower.cpp:128-135` (`leaveIdle`) and `core1.cpp:104`: post
  `RING_DRAINED`, clear `plannerActive`, write nothing.
* The session's JOGGING default closer: on `RING_DRAINED`, take
  `plannerLock`, recheck the ring is empty, write IDLE. A block pushed in
  between keeps the session open; Core 1 picks it up as it does today.
* JOB keeps its open job on a dry ring (BUFFERING, JB1); `cancel` stays its
  exit.

**Tests (native):** drained with an empty ring closes; drained with a block
pushed meanwhile stays open.

**Checks:** as R1. Human: step jogs, `jogto`, a continuous jog and its stop,
a job run dry, then cancel.

**Depends on:** R2a.

### Status

Planned.

## Branch R3: `refactor/sequences`

### Plan

**Type:** refactor. No new commands.

**Purpose:** ring-move and wait steps, and the sequence driver.

* `ops/runner.{h,cpp}`: step kinds ring move (ends on `RING_DRAINED` from the
  observer) and wait (ends on a named event or a timeout); one kind per step.
* `controller/seq/sequence.{h,cpp}` (new): a fixed step list, one owner, the
  default failure policy, `stop` handling, the session close at the end.
* `core0.cpp:168-185`: loop order observe, planes, runner, session,
  sequences.
* A sequence-owned session takes the owner's events; a session without one
  uses the default closers (R2a, R2b).

**Tests (native):** a full run (legs, ring move, wait); a leg failing; a ring
move with `stop`; a wait timing out; a stale generation dropped.

**Checks:** as R1. Human: nothing new is reachable; R2's checks again.

**Depends on:** R2b.

### Status

Planned.

## Branch R4 (optional): `refactor/home-sequence`

### Plan

**Type:** refactor. Behaviour unchanged.

**Purpose:** `home` as a sequence on the driver.

* `controller/seq/home.cpp` and `homingTick` (`ops/homing.cpp:101`) rebuilt
  on R3, with `ALARM_HOMING_FAIL` and `homefail=` as the policy.
* Only if R3 reads well; homing works and is bench-sensitive.

**Checks:** as R1. Human: `home` on the machine, a failing switch.

**Depends on:** R3.

### Status

Optional.

## Features

In docs/plans/motion-sessions.md; their sections are revised to this plan.

| Branch | On this plan | Depends on |
|---|---|---|
| J1b `feature/jogto-named` | first sequence: park Zs (legs), XY line (ring move), owner JOGGING, reason `JOGGING_GOTO` | R3 |
| JB2 `feature/tool-mount` | `mount` also posts the event the tool-change wait takes | nothing |
| JB3 `feature/layer-header` | unchanged | JB1 (done), JB2 |
| JB4 `feature/tool-change` | second sequence: park Zs, per head an XY line to load and a wait for `mount`, owner `JOB.TOOL_CHANGE` | R3, JB3 |

## Later

* Probe legs (`ops/probe.cpp`) as a leg kind; probing in a job
  (`TOOL_PROBE`) as a step of the tool-change sequence.
* `cjogTick` (`data_plane.cpp`) out of the plane, into the session.
* Recovery sequences: `unalarm` picks one from `alarmReason`
  (docs/plans/state-handling.md, `unalarm`). A feature on R3.

  | Reason | Sequence | Step kinds |
  |---|---|---|
  | `ESTOP` | `unstop`, wait for the make-safe replies | wait |
  | `NODE_FAULT` | `axis_map` retry, pending axes first | primitive |
  | `BUS_DEGRADED` | `bus_exclude` the mute nodes the strictness allows | primitive |
  | `LIMIT_LATCHED` | reverse leg off the switch | legs |
  | `HOMING_FAIL` | `home_end` | primitive |
  | `PROBE_FAIL` | `probe_end` | primitive |

  * The owner state is ALARM; a recovery runs inside it.
  * Failure keeps the same alarm, with the reason in the reply; success
    closes through `resumeOrHold`.
  * One reason per `unalarm`: a recovery that exposes another alarm stops
    there.

## Open

* Wait events: a fixed enum (`mount`, …) or an id carried by the event.
* Whether motion entry (IDLE → JOGGING/JOB) also moves to the session at
  admission, which would leave Core 1 writing only in-session reasons and
  alarms.
