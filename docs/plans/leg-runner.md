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

**Status:** planned.

## Later

Not yet branches; recorded so the shape is not lost.

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
* Unlatched seek (no alarm, no re-zero, reports the hit or not found) for leg
  span; node park outside homing; tool change as a recipe.
* `cjogTick` (`data_plane.cpp:415`) is session logic living in the plane.
* Probe legs (`ops/probe.cpp`) as a second leg kind.
