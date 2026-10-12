# Leg runner: active objects on Core 0, starting with legs

Core 0 is a super loop of polled ticks (`core0.cpp:168`). Node legs are
owned by homing: `homingTick` is the only caller of `legPoll` and drops every
leg outside `STATE_HOMING` (`ops/homing.cpp:116-128`). Node park, leg span and
tool change (docs/plans/motion-sessions.md) all need legs outside homing.

This plan tries the active-object pattern on the legs only. If it reads and
behaves well, the rest of Core 0 moves to it later (see Later).

## Decisions

* Active object: owns its state and a mailbox; others post events to it and
  never touch its state; it handles one event at a time, to completion, and
  never blocks on a leg or a move. Scheduled cooperatively by the existing
  loop. Events are small structs copied by value.
* A **step** is one or more legs that run together. It is done when every leg
  is done; it fails when any leg fails, and the runner then aborts the step's
  other legs. One leg per node within a step.
* The runner applies the physical effects of a leg ending (origin invalidate,
  or adopt for a finished park; today `homingRelease`, `ops/homing.cpp:33`),
  whoever armed it. Policy (latch, `HOMING_FAIL`, reasons) stays with the
  client.
* Results are posted to a mailbox: step done, step failed, aborted, each with
  its `LegEnd`s. Every event carries a generation, bumped when a client opens
  the runner; events of an older generation are dropped. Overflow is a bug and
  asserts.
* `abortAll` and `dropAll` stay direct calls; the runner never calls back.
* The bus and the clock are reached through a seam (arm, park, abort, status,
  now), so the runner runs in `native` against a scripted fake node.

## Branch 1: `refactor/leg-runner`

**Type:** refactor. Homing behaves as today.

**Purpose:** steps of legs with a result mailbox, homing as the first client,
and native tests for the runner.

**Files:**

* `src/rp2350/core0/ops/leg.{h,cpp}`: the leg table and per-leg poll stay;
  `legArm`/`legArmPark`/`legAbort`/`legAbortAll`/`legPoll` reach the node
  through the seam instead of `core1_rpc` and `millis()` directly
  (`leg.cpp:140,148,205,212,245,260,295,320`).
* `src/rp2350/core0/ops/runner.{h,cpp}` (new): step queue, mailbox,
  generation, physical effects.
* Seam (new): bus interface plus clock; Pico implementation over
  `core1_rpc`, fake in `test/`.
* `src/rp2350/core0/ops/homing.cpp`: pops the mailbox instead of `legPoll`;
  keeps latch, `HOMING_FAIL`, `homingReason`. The `STATE_HOMING` drop stays
  until a session layer exists.
* `src/rp2350/core0/controller/seq/home.cpp`: each phase is armed as one step.
* `cmd/axis.cpp:503` (`leg`) arms one-leg steps through `homingBegin`;
  unchanged on the wire.
* `test/test_runner/` (new), added to `test_filter` in `platformio.ini`
  `[env:native]` (`:442`).

**Tests (native):** settle polls, poll misses, deadline, two legs in one step,
one failing and the other aborted, park adopt vs invalidate, stale
generation dropped, mailbox bound.

**Out of scope:** session layer, `cmd/` admission, probe legs (Core 1 RPC),
ring moves, waits, unlatched seek, node park as a feature.

**Checks:** `pio run -e pico`, `pio test -e native`. Human scope: dummy legs
and a real `home` on the bench behave as before.

**Exit:** if the event code reads worse than the polling it replaces, keep
the runner and the seam, and drop the mailbox.

**Overlap:** `platformio.ini` (`test_filter`).

**Depends on:** nothing.

**Status:** merged (faad490). Unblocks node park and leg span as the runner's
next client. `pio run -e pico` and `pio test -e native` pass.
Bench: `home`, parallel dummy legs, a failing dummy, parallel raw parks,
`leg_abort` and `stop` mid-leg behave as before. A raw seek or retract onto
a switch was not run on its own (`home` runs the same legs).

**Outcome:**

* One live step, no queue: no step depends on nothing before it, so a
  lookahead had nothing to queue. A raw `leg` joins the live step, so raw legs
  still run in parallel. Arming stays synchronous; refusals come back from the
  arm call.
* The seam is link-time on `leg.h` (already native-clean): the test fakes the
  leg layer. `leg.cpp`'s bus logic is still bench-tested only; no `ipc/`
  changes.
* Origin effects stay in `homing.cpp` (`homingRelease`): `position.h` pulls in
  `core1_rpc.h` and would not build natively. Move them with a second client.
* `homingTick` ticks the runner, after its `STATE_HOMING` drop check, so a
  dropped leg is never polled; the loop does not call `runnerTick`.
* `runnerAbort` returns the released end directly, as `legAbort`, so
  `leg_abort` alarms in the same pass; only the step end is posted.
* `home.cpp` unchanged: `armPhase` arms a phase while nothing is live, so each
  phase is already one step.
* Mailbox verdict: neutral for now. It is drained right after the tick that
  fills it, and adds two subtleties (an abort's step end drained while idle;
  `runnerAbortAll` clearing the box mid-pop). Judge it at the next client:
  keep it if that client and `homeTick` read events with less code than they
  poll with; otherwise return the end from `runnerTick` and drop the box.

## Leg intents and states

Decided in planning after branch 1; branches 2 and 3 build it.

* `leg <intent> <node> …`: the intent comes first, then the node. The intent
  decides the leg's end effects (latch, origin, alarm) and the state it may run
  in. `leg_abort` is unchanged.

| Intent | Ends with | Origin |
| --- | --- | --- |
| `seek` | on the switch: latch set; budget out: `ALARM_HOMING_FAIL` | dropped |
| `retract` | off the switch: latch cleared | dropped |
| `sweep` | rotary index found | dropped |
| `park` | on target; off target: `ALARM_HOMING_FAIL` (`LEGFAIL_PARK`) | adopted |
| `span` | on the switch: latch set, `ALARM_LIMIT_LATCHED`; budget out: no alarm | adopted |
| `unlatch` | off the switch: latch cleared, `resumeOrHold` | adopted |
| `probe` | as `probe_leg` today | as today |

* `probe`'s node is a check, not an input: it must be the Z that `probe_map`
  bound (`err node <n> not_probe_z`), as `intendedRetract` is checked. The
  binding stays the one source of truth. `probe_map` and `probe_end` stay the
  session commands that remap and restore the slots.
* `span` measures travel: the node's leg span (`nodestat … span`) is read
  after it ends. A hit is visible as `ALARM_LIMIT_LATCHED`, not found as IDLE.
* Adopted means the origin is re-derived from the node's counter
  (`originAdopt`) if the node still has one; a node without one stays
  un-homed.
* `STATE_TRAVERSE = 8` (new): node legs that keep the origin, from IDLE. It
  opens on the first leg and closes itself when the step ends, through
  `resumeOrHold`. No reason byte. Core 1 leaves it alone (it acts only on
  IDLE with a ring, PAUSED, RUNNING, JOGGING, ESTOP: `core1/core1.cpp:97-114`).
* The state a leg is armed in is held until its step ends: transitions that
  Core 0 commands would make (`home_end`, `setorigin`, `resume`, jogs, records)
  answer `err busy` while a step is live. Faults (`stop`, bus, node) still take
  the state, and the runner drops the step.

Arming:

| Current state | Intent | Transition to |
| --- | --- | --- |
| IDLE | seek, retract, sweep | HOMING |
| IDLE | park, span | TRAVERSE |
| HOMING | seek, retract, sweep, park | stays HOMING |
| TRAVERSE | park, span | stays TRAVERSE (joins the step) |
| ALARM (limit latched) | retract | HOMING |
| ALARM (limit latched) | unlatch (latched nodes only) | TRAVERSE |
| PROBING (opened by `probe_map`) | probe | stays PROBING |
| PAUSED | park | stays PAUSED |
| TRAVERSE, PROBING with a leg live | anything else | `err busy` |
| everything else | any | `err bad_state` |

Step end:

| State | Outcome | Transition to |
| --- | --- | --- |
| TRAVERSE | all succeeded, nothing latched | IDLE (`resumeOrHold`) |
| TRAVERSE | span hit | ALARM (limit latched) |
| TRAVERSE | span not found | IDLE |
| TRAVERSE | park or unlatch failed | ALARM (homing fail) |
| HOMING | all succeeded | stays HOMING, waiting |
| HOMING | any failure | ALARM (homing fail) |
| PAUSED | park succeeded | stays PAUSED |
| PAUSED | park failed | ALARM (homing fail), job kept |
| any | `stop` or another fault | the fault's state; the step is dropped |

## Branch 2: `refactor/leg-owner`

**Type:** refactor. Behaviour as today.

**Purpose:** let a second client own legs: the runner records who armed the
live step, is ticked once from the loop, drops the step when its owner loses
the state, and applies end effects by intent.

**Files:**

* `src/rp2350/core0/ops/runner.{h,cpp}`: an owner (state at arm) on the live
  step; the drop check moves here from `homingTick` (`ops/homing.cpp:146`);
  events go to the owner.
* `src/rp2350/core0/ops/leg_effects.{h,cpp}` (new): end effects by intent;
  `homingRelease` and the latch half of `homingLegEnd` (`ops/homing.cpp:34`,
  `:118`) become its rows. Linked out natively, as `leg.h` is.
* `src/rp2350/core0/ops/homing.cpp`: keeps latch policy, `homingFail`,
  `homingReason`; stops ticking the runner.
* `src/rp2350/core0/core0.cpp:168`: ticks the runner.
* `test/test_runner/`: owner routing, drop on lost state.

**Checks:** `pio run -e pico`, `pio test -e native`. Human scope: `home`,
raw legs, `leg_abort`, `stop` mid-leg as before.

**Depends on:** branch 1.

**Status:** merged (45b32ce). Unblocks branch 3. `pio run -e pico` and
`pio test -e native` (86/86) pass. Bench: `home`, parallel dummies, a failing dummy, parallel
parks, `leg_abort` mid-park and `stop` mid-park behave as before.

**Outcome:**

* The runner keeps each leg's intent per node and copies nothing into
  `LegEnd`; `leg.h` is unchanged. `homingBegin` derives the intent: index
  → sweep, `intendedRetract` → retract, else seek.
* Effects split in two: `legEffectEnd` (a verdict: latch on success, then
  release) and `legEffectRelease` (abort, drop: release only), so a dropped
  seek never latches.
* The owner check is a seam too (`legOwnerHolds`, in `leg_effects`), since
  `machineState` is not native-clean.
* `homingAbort` still checks HOMING before failing the session: the runner
  has released the leg, and the verdict is policy.
* One owner per step, and the owner drains the box; `runnerPop` takes no
  owner. Branch 3 decides whether TRAVERSE needs it.
* `homingAbandon` drops its node's origin itself, as no leg ended.

## Branch 3: `feature/leg-intents`

**Type:** feature.

**Purpose:** the intents and states above; node park and leg span from IDLE.

**Files:**

* `src/rp2350/ipc/shared_state.h:129`: `STATE_TRAVERSE`.
* `src/rp2350/core0/cmd/axis.cpp:407-510`: `cmdLeg` parses
  `leg <intent> <node> …`; `legGateDenies` becomes the arming table.
  `cmdProbeLeg` (`:853`) folds in as `leg probe <node> …`, the node checked
  against the session's Z; `probe_map`/`probe_end` stay. Transition commands answer `err busy` while a step is live.
* `src/rp2350/core0/ops/leg_effects.cpp`: rows for `span`, `unlatch`.
* `src/rp2350/core0/ops/homing.cpp`, a TRAVERSE owner (new, beside it):
  step-end transitions.
* `src/rp2350/core0/controller/seq/home.cpp`: unchanged on the wire (it arms
  through `homingBegin`).
* Web: `web/src/wire/link/commands.ts:428,457,478,559`, the sim parser
  (`web/src/wire/link/backends/sim.ts:723`), `MachineState.TRAVERSE` in
  `web/src/wire/format/status.ts`.
* Docs: `docs/homing.md` §2.2, `docs/wire_protocol.md`.

**Checks:** `pio run -e pico`, `pio test -e native`, `pnpm typecheck` and
`pnpm test` in `web/`. Human scope: park and span from IDLE, a span hit then
`unlatch`, a span not found, `stop` mid-traverse, `home` and web homing as
before.

**Mailbox:** judged here (see branch 1's Outcome).

**Overlap:** `web/src/wire/`, `shared_state.h`.

**Depends on:** branch 2.

**Status:** not started.

## Later

Not yet branches; recorded so the shape is not lost.

* `unalarm` runs `unlatch` on every latched node itself (needs the config's
  homing figures, so it lives in `controller/`).
* Probing inside a job. Invariant: **TOOL_CHANGE never changes the slot map;
  only TOOL_PROBE does, and it restores the job's map on every exit before
  leaving**, as the probe session already does (`ops/probe.cpp:130-160`;
  unbinding keeps origins, `ops/slot_map.cpp:58-96`). One head at a time:

  ```text
  TOOL_CHANGE (enter)  park both heads' Z (park legs, in parallel)
    head 0: TOOL_CHANGE  XY line to its load spot, wait for `loaded`
            TOOL_PROBE   map Z0 + vacuum, probe, restore the job map
    head 1: TOOL_CHANGE  …
            TOOL_PROBE   …
  → BUFFERING
  ```

  The status carries the head beside the reason. A head already holding its
  tool is skipped. TOOL_PROBE is optional (manual touch-off in TOOL_CHANGE).
  A failed probe restores the map and alarms with the job kept; after
  `unalarm` the head returns to TOOL_CHANGE. Core 1 treats TOOL_PROBE as it
  treats PROBING today.

* Layers: observer (Core 1 signals to events; today `reconcileValidity`,
  `ops/position.cpp:355`) → planes (`cmd/`, `controller/cmd/`: parse, reply)
  → session (`ops/session`: state, admission, the only Core 0 state writer,
  abort fan-out) → recipes (`controller/seq/`: config-aware sequences that
  own a session) → runner → mechanisms (`ops/*`). Dependencies point down;
  upward news goes by mailbox.
* Reads (`get`, queries, relays) skip the session.
* Loop order: observe, planes, runner, session, recipes.
* Steps grow waits (event or time) and ring moves (pushed to Core 1, done
  when the ring drains at rest). Legs and the planner ring never move at the
  same time.
* Tool change as a recipe.
* `cjogTick` (`data_plane.cpp:415`) is session logic living in the plane.
* Probe legs (`ops/probe.cpp`) as a second leg kind.
