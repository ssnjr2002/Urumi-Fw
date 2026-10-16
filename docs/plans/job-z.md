# Z in jobs

Lifts, plunges, tool on/off and duty in a job. Follows
docs/plans/motion-sessions.md (states, records, the layer header, JX's
feeds); the decisions it holds on Z are restated here, and this doc is the
reference for them from now on.

A is not implemented, so corner lifts (swivel at a `BREAK` over
`cornerAngleDeg`) and cable-range unwinds wait for A and have no branch
here. Until then a `BREAK` runs through without a lift.

## Decisions

### Heights

* **The Pico inserts every lift;** the wire stays 2D. Lifts and plunges are
  Z-only lines (a change of axis set stops), tagged with the index of the
  record they lead into, so a resume's `LAYER_HDR(N)` replays its own travel.
* Work Z is the Z where the head's tip meets the material
  (coordinate-system.md): `WPos` Z 0 is the material's top, + up out of it,
  − into it. In machine Z, with the mesh applied by its rule:

  ```
  lift / travel   workZ + c
  plunge          workZ − k·t          k in [0, 2]; 1 a full cut, past 1 into the mat
  ```

  `t` (material thickness) from the layer header, `k` (plunge scale) from the
  layer header, `c` from `machine.zClearance` (machine-wide; renamed from
  `clearanceMm`, which only the host read). `t` is a u16 in µm
  (to 65.5 mm; Z travels 48 mm, so preflight bounds it first). A tool with
  `plunge: false` (a pen) has depth 0 whatever `k` says.
  A head with no work Z set refuses the layer header (`NACK_JOB`).
* **Feeds:** a plunge at the tool's `plungeFeed`, else
  `machine.feed.plungeFallback`; a lift at `machine.feed.liftFeed` (new,
  machine-wide, ≥ 1).

### What lifts

| cause | stop | where | branch |
|---|---|---|---|
| contour `END` | lift | after every contour's last record | ZA |
| contour `START` | travel, plunge (a lift first if still down) | every contour start | ZA |
| pause, abort | lift | where it stopped, mid-contour too | ZA |
| lost host | lift | where it stopped, after a timeout | later |
| duty, past `maxOnS` | lift, tool off, `DWELL`, tool on, plunge | a corner, else anywhere | ZC2 |
| corner over `cornerAngleDeg` | lift, swivel, plunge | that `BREAK` | with A |
| cable range | lift, unwind, plunge | anywhere | with A |

Estop does not lift. Every other alarm lifts when Z can still move, turns the
tool off and keeps the vacuum on.

* `END` is the only lift the Pico adds by itself in ZA; it covers job end
  and a ring that runs dry between contours. A ring dry mid-contour does not
  lift. A resume comes in as a `START`: travel to the resume point less the
  margin, plunge, continue.
* The lift and plunge separate the travel from the cuts on either side, so
  the corner blend between a travel and its first cut (kept by JX) goes.
* Pause and abort lift before their state lands; Core 0 inserts the lift,
  Core 1 only runs the ring:
  * pause: `HOLDING`; Core 1 brakes to rest, keeps the resume point, drops
    the ring and posts `FACT_HELD` (new), staying in `HOLDING`. Core 0 queues
    the lift; on its `FACT_RING_DRAINED` Core 0 lands `PAUSED`.
  * abort: `ABORTING`; the finished abort's `FACT_RING_DRAINED` has Core 0
    queue the lift; the next drain closes the job to IDLE.
  * motion-sessions.md's `HOLDING → PAUSED` row: writer Core 0, not Core 1.
* A resume plunges at the resume point (`s − margin`).

### Tool on and off

* **Event blocks in the ring**, executed by Core 1 at rest (where it owns the
  bus): motion stops before the plunge, tool on, wait `settleS`, plunge; the
  lift runs with the tool on so the blade does not stick, and the tool goes
  off at clearance.

### Duty

* The existing `dutyLimits` (docs/tool_duty_limits.md): the tool is on only
  while cutting, from the plunge's event to the lift's. On-time adds up
  across contours until a reset; an off of at least `dwellS` is a reset. At a
  contour's `START`:
  * the off time since the last lift is already ≥ `dwellS`: reset;
  * else, if on-time ≥ `minOnS`: wait until the off time reaches `dwellS`
    (`DWELL`), reset;
  * else: plunge without a reset.
* Mid-contour, past `maxOnS`: the duty lift, `DWELL` for `dwellS`.
* **A lift can happen anywhere on the path:** hold to rest, lift, plunge back
  at the same XY, continue the rest of the block from `s`. Core 0 inserts the
  lift and plunge at the ring's front after `restartFrom(s)`. A START with
  on-time ≥ `minOnS` dwells anyway; mid-path, the stop goes at the latest
  corner that fits (`nextBreakDist`, ZC1), else at a deadline Core 1's live
  counters trigger early enough to stop in time (ZC2).

### Out of range

* Preflight checks the lift and plunge heights against Z's soft range over
  the bbox, with the mesh; a failure is `NACK_PREFLIGHT`.

## Branches

A dependent chain, run as a Single Session.

### ZA: lifts and plunges

* Name: `feature/job-lifts`
* Purpose: a lift at every contour `END`, travel and plunge at every
  `START`, a lift before `PAUSED` and before an abort's IDLE; `t` on the
  wire, `k` live, work Z required.
* Interface: `LAYER_HDR` gains `t` (u16 µm, so 22 bytes); `machine.feed.liftFeed`;
  `machine.zClearance` (was `clearanceMm`, the old key refused by name); blob and schema version 8.
* Files:
  * `src/rp2350/core0/usb_protocol.h:33` (layout), `data_plane.cpp`,
    `ops/layer.{h,cpp}`
  * `src/rp2350/core0/planner/queue.cpp:433` (the `START` travel)
  * `src/rp2350/core0/config/config_decode.{h,cpp}`
  * `lib/planner/` if a Z-only line needs it
  * `web/src/wire/format/layer.ts`, `constants.ts`, `link/backends/sim.ts`
  * `web/src/machine/schema.ts`, `defaults.ts`, `json/load.ts`, `json/validate.ts`, `json/blob.ts`
  * `config/controller.jsonc`, tests and fixtures, `docs/wire_protocol.md`
* Depends on: JX (merged).
* Status: Done (merged 0a9a61b). Unblocks ZB.
* Outcome:
  * Deviations: `c` is the machine-wide `machine.zClearance` (the host's
    `clearanceMm` renamed and sent to the Pico), not a per-tool `clearance`;
    a bad value is `cfgerr=z_clearance`. The work-Z check applies only with
    the tool mounted: a header that starts a tool change is not refused (the
    change unsets work Z, and the header comes again after the Z zero).
  * A START's first Z move goes to clearance from wherever Z is, down as well
    as up (at `plungeFeed` going down). The END lift is tagged with the next
    record, so a pause in it resumes at that record's START.
  * Pause: `pauseRequested` stays set until Core 0 lands PAUSED, so records
    keep `NACK_PAUSED`. A pause at rest in BUFFERING (a ring dry mid-contour)
    goes the same way and lifts. Abort: `machinePublish` queues the lift once
    and closes the job when it has drained; records get `NACK_ABORTING`
    through it. Core 1 runs a ring entered in HOLDING or ABORTING as a lift
    (no pause, no rebuffer, drains with `FACT_RING_DRAINED`).
  * Interfaces for ZB/ZC: `LayerMotion {cutFeed, plungeFeed, depth}` from
    `layerResolve`; `plannerOpenLayer(const LayerMotion&, …)`;
    `plannerQueueLift(why)`, `plannerLandPaused()`; `FACT_HELD`.
  * Bench (2026-10-16, air zero at work Z −20, t 2 mm, k 1): two squares ran
    clearance → travel → plunge to −22 → cut → lift to −18 each; a pause
    mid-cut held, lifted at the stop point, then PAUSED (resume 0/6) and
    resumed with a plunge; an abort mid-cut lifted, then IDLE. Not run: the
    `NACK_JOB` for a head without work Z (needs a tool change).
  * Out of scope: the web sim (`wire/link/backends/sim.ts`) lands PAUSED
    without the HOLDING lift. The main folder's `config/controller.jsonc`
    gains `liftFeed`/`zClearance` only with this merge; the Pico on the bench
    already runs config v8.

### ZB: tool events and duty at START

* Name: `feature/job-tool-events`
* Type: `feature/`.
* Purpose: tool on/off event blocks, `settleS`, the duty counter and `DWELL`
  at contour starts.
* Config: a tool is **active** (powered) with a `power` block, **passive**
  (pen) without one; only an active tool gets events and duty.

  ```jsonc
  "power": {
    "node": 8,            // bus id; the command from that peripheral's type
    "settleS": 0.3,       // on → plunge
    "dutyLimits": { "maxOnS": 30, "minOnS": 20, "dwellS": 2 }   // optional
  }
  ```

  `power.node` must be a present peripheral of a switchable type (today
  `NODE_TYPE_KNIFE_OSC` → `CMD_KNIFE_OSC`), else the config is refused, on
  the host and the Pico. The tool-level `dutyLimits` is refused by name (as
  `clearanceMm` was). Blob and schema version 9.
  * Bench stand-in: with the build flag `ACCEPT_LASER_AS_POWER_TOOL` (off by
    default) the Pico also accepts the machine's `laser.node`, switched by
    `CMD_LASER`. The host validator accepts both; a Pico without the flag
    refuses the laser node. While the knife node is off the bus, the
    knife's `power` block in `config/controller.jsonc` is commented out; a local bench config
    gives it `power.node` = the laser node.
* Events: a new `planner::Block::Kind EVENT` (`TOOL_ON`, `TOOL_OFF`), zero
  length, at rest on both sides (entry and exit 0, no blend), tagged like the
  moves so a resume replays it. The Pico inserts every event; the wire does
  not change.
  * START: Z to clearance, travel, `TOOL_ON`, plunge, curve (5 blocks, was 4).
  * END: lift, `TOOL_OFF` (2, was 1).
  * Pause and abort: the lift runs with the tool on, then `TOOL_OFF`
    (`plannerQueueLift`). A held job with the tool already off lifts alone.
  * Core 1 runs an event at rest, where it owns the bus: the node command
    direct from the follower. A failed command (timeout, NAK) retries 3 times
    at rest, then alarms (`F_TOOL`, 0x0100, a new event fault; Core 1 reports
    it as a fact). A failed `TOOL_ON` leaves the head at clearance,
    unplunged; a failed `TOOL_OFF` sends `CMD_MAKE_SAFE` to the node first.
  * An event fault in a job tears it down through the abort path
    (`faultSet` → `jobAbort`), which lifts: the tool goes off after the lift
    there too. Where no lift can run (Z's soft range, no Z limits), the
    `TOOL_OFF` is queued alone. Estop's sweep turns it off at once.
* Duty: Core 1 keeps the on-time and the last off instant (live counters,
  also for ZC). `TOOL_ON` applies the rule under Duty when it runs, with the
  live counters: wait if due (`DWELL`), reset, tool on, wait `settleS`. Core 0
  reads no counters; a START that needs no wait costs nothing. The off time
  is wall time and runs through `PAUSED`, so a pause of at least `dwellS`
  counts as the dwell: a resume normally waits `settleS` only. Counters reset
  only with a job's open, a tool change and an alarm.
* **`JOB_DWELL`** (new reason, 7, after `TOOL_WAIT`; motion-sessions.md's
  `RUNNING → DWELL → RUNNING`, writer Core 1): published while `TOOL_ON`
  waits for the duty, back to `RUNNING` when it ends. Vacuum stays on. A
  `pause` in `DWELL` ends the wait and lands `PAUSED` with no lift (Z is at
  clearance, the tool off); the resume's `TOOL_ON` applies the rule again.
  Web: the reason name, the sim's state.
* Files:
  * `lib/planner/planner/planner.h:48` (`Kind`), `planner.cpp` (push an
    event, forced rest), `executor.{h,cpp}` (stop at an event, report it);
    native tests
  * `src/rp2350/core0/planner/queue.cpp:417` (`plannerQueueRecord`, ring
    room at :443), `:486` (`plannerQueueLift`)
  * `src/rp2350/core1/emit/follower.cpp` (run an event), `core1/bus/` (send
    from Core 1), duty counters shared via `ipc/`
  * `src/rp2350/core0/config/config_decode.{h,cpp}` (`CfgTool` gains power,
    :145, :228, :301; `CFG_SCHEMA_VERSION` 9)
  * `web/src/machine/schema.ts:482–530` (`DutyLimits` into a `Power` type),
    `json/load.ts:218`, `:801`, `json/validate.ts:236`, `json/blob.ts`; `JobReason` in `web/src/wire/`;
    `ipc/shared_state.h` (`JOB_DWELL`); `ops/machine.h` (`F_TOOL`) and the
    web fault names; `platformio.ini` (the flag, documented in AGENTS.md)
  * `config/controller.jsonc:211`, fixtures and tests,
    `docs/tool_duty_limits.md` (marked superseded by this doc; its host
    scheduler is already gone from `web/src`), `docs/wire_protocol.md`
* Out: the blower, the alarm lift ("Every other alarm lifts"), the lost-host
  lift.
* Depends on: ZA.
* Status: Done (merged e718c32). Unblocks ZC1.
* Outcome:
  * Deviations: `power.node` is `{ id, type }`, the command from the type
    (3 → `CMD_KNIFE_OSC`, 1 → `CMD_LASER` with the flag); no separate DWELL
    block: `TOOL_ON` applies the duty on Core 1 (a START is 5 blocks, an
    END 2). The tool goes off after the lift on END, pause, abort and event
    faults; estop's sweep makes it safe at once. `cancel` from a dry
    buffering job switches a tool left on off (it still does not lift).
  * Interfaces for ZC: `jobPower` (Core 0 writes at a layer open),
    `dutyEpoch` (bumped on a job open, a tool change, `faultSet`), `toolOn`
    (Core 1's); `TOOL_EV_ON`/`TOOL_EV_OFF` events; `F_TOOL` 0x0100,
    `JOB_DWELL` 7; `busNodeCmd`, `busNodeSafe`.
  * Bench (2026-10-16, laser on node 3 as the knife's power, flag on,
    settleS 0.5, minOnS 3, dwellS 5): the laser came on before each plunge
    and off at the end of each lift; settle 0.43–0.52 s; no DWELL after a
    cut under minOnS; DWELL after a longer one until 4.95–5.0 s off; a pause
    counted as off time; a pause in DWELL landed PAUSED with no Z motion;
    estop made node 3 safe; a new job's first start had no DWELL. A Pico
    without the flag refuses the laser node, and any Pico an absent node
    (`cfgerr=power`). Not run: a failed switch (`F_TOOL`), since node 3 is
    also Z.
  * Out of scope: the web sim has no tool events; `pio run -t uploadfs`
    encodes the config without the host validator, so only the Pico refuses
    a bad one; `pio test -e native`'s `test_machine` already fails to link
    on main.

### ZC1: the next break's distance on the wire

* Name: `feature/wire-break-distance`
* Type: `feature/` (a protocol change, so it merges before ZC2).
* Purpose: the host tells the Pico how far each stretch runs before its next
  corner, so the Pico can stop for duty at the latest corner that fits.
* Interface: `BEZIER` grows to 58 bytes:

  ```
  [55..56] nextBreakDist  u16 mm: on a START or BREAK record, from its p0 to
           the next BREAK or END of the contour; 65535 = at least that far;
           0 = unknown. 0 and ignored on other records.
  [57]     CRC8 over bytes [0..56]
  ```

  u16 mm: the useful range is `maxOnS × cutFeed` (the knife's 30 s at
  100 mm/s is 3 m); 1 mm is within the trigger's margin at cut feeds.
* The Pico checks the size and stores the field on the block (a `Block`
  field, `lib/planner/planner/planner.h:47`); nothing reads it yet.
* Files:
  * `src/rp2350/core0/usb_protocol.h:18` (layout), `data_plane.cpp:186`
    (pass it to `plannerQueueRecord`), `core0/planner/queue.{h,cpp}`
  * `lib/planner/planner/planner.h` (the block field), native tests
  * `web/src/wire/format/bezier.ts`, `constants.ts:14`, `link/backends/sim.ts`
  * `web/src/toolpath/annotate.ts:277` (compute it per contour), tests and
    fixtures
  * `docs/wire_protocol.md`
* Depends on: ZB.
* Status: Done (merged b430a53). Unblocks ZC2.
* Outcome:
  * Deviations: every block of a record carries the field, through the tag
    (`setTag(layer, record, breakDist)` → `Block::break_dist`); the END
    lift's re-tag carries 0. A resumed record stores `ceil(d − from)`, 0 and
    65535 as sent; `d` spans the record, so this stays positive. The host
    rounds up. `sim.ts` needed only the size.
  * Interface for ZC2: `Block::break_dist`, u16 mm from the record's start
    (or resume point) to its next BREAK or END; read it on the record's
    first block.
  * Out of scope: `fish.svg`, which `web/test/port/cppRefBezier.test.ts`
    regenerates `test/data/bezier_records.txt` from, was removed in eb51697;
    restored by `fix/fish-svg` (3ca660e). `test_machine` still fails to link,
    as on main.

### ZC2: mid-path duty lifts

* Name: `feature/job-midpath-lifts`
* Type: `feature/`.
* Purpose: when a contour would run past `maxOnS`, stop (at a corner where
  one fits), lift, `DWELL`, plunge back at the same XY and continue the rest
  of the block from `s`.
* A START already moves duty lifts early: ZB's rule dwells at any START with
  on-time ≥ `minOnS`. A mid-path lift is only for a contour that runs from
  under `minOnS` past `maxOnS`.
* **The break's cost** (docs/tool_duty_limits.md §5): braking and the lift
  are on-time of the window ending; settle, plunge and the ramp back up are
  the next window's, counted by ZB's counters from the switch on.
* **Core 0's on-time estimate:** Core 0 keeps its own count as it queues
  records: each record adds its cut time (length / cutFeed); a START whose
  count is ≥ `minOnS` will dwell, so the count goes back to 0; a shorter
  START carries it on. It errs only towards stopping early. Core 1's live
  counters drive only the deadline.
* **At a corner** (planned, all on Core 0): when Core 0 queues a BREAK record
  with its count past `minOnS`, and the next corner is certainly too late,
  `count + nextBreakDist/cutFeed + dutyLiftS + marginS > maxOnS`, it pushes
  lift, `TOOL_OFF`, `TOOL_ON`, plunge at the ring's tail before the record's
  curve, as a START does, and the count goes back to 0. A Z move after an XY
  curve is a junction of 0 (`line.cpp:63`), so the stop is planned, not
  braked. Otherwise the next corner decides again. `nextBreakDist` 0 never
  stops at a corner. The blocks carry the record's tag, so a pause in them
  resumes at the corner.
* **At the deadline** (the fallback: a long smooth stretch, an estimate the
  accel beat): Core 1 holds when `onTime + v/a + dutyLiftS + marginS ≥
  maxOnS`, with the tool on and in a cut; `onTime` is `dutyOnMs + (now −
  toolOnAtMs)`.
  1. Core 1 brakes to rest (`hold()`).
  2. Core 1, Held, under the lock: a new `Executor` method (`resume()`
     without its replan and commit) calls `restartFrom(s)` and resets the
     executor. Core 1 stays `RUNNING`, posts `FACT_DUTY_HELD` (new) and stops
     ticking until Core 0 clears a flag.
  3. Core 0: `Planner::insertFront` (new) with lift, `TOOL_OFF`, `TOOL_ON`,
     plunge at the held XY, then `replanAndCommit()`, then clears the flag.
  4. Core 1 runs on: ZB's `TOOL_ON` dwells (`JOB_DWELL`) and resets the
     counters.
  The inserted blocks carry the held block's tag and `Block::resume_s` (new,
  set through the tag) = `s`; `landHeld` keeps `resume_s` for a LINE or
  EVENT block, so a pause in them resumes at `s − margin`, not the record's
  start.
* `dutyLiftS`: the lift's time (`depth + zClearance` at `liftFeed`, Z's
  accel), Core 0's at a layer open, in `jobPower` with `maxOnS` (already
  decoded, `CfgPower.maxOnS`). `marginS` (~50 ms, a firmware constant)
  covers Core 0 picking up the fact; a Core 0 stall past it overruns
  `maxOnS` by the stall.
* Ring room: a corner stop's 4 blocks go in with the record's
  (`plannerQueueRecord`'s full check, `queue.cpp:457`); the full check also
  keeps 4 slots free for a deadline insert, for a tool with `dutyLimits`.
* Soft range: Core 0 checks the lift and plunge at the stop's XY
  (`zMoveFits`): at a corner it refuses the record (`PQ_SOFT_LIMIT`); at the
  deadline it is `F_TOOL` and the job aborts.
* Files:
  * `lib/planner/planner.{h,cpp}` (`insertFront`, `Block::resume_s`),
    `executor.{h,cpp}` (the release at Held); native tests
  * `src/rp2350/core0/planner/queue.cpp` (the count, the corner stop, the
    insert, ring room, `dutyLiftS`), `core0/ops/observe.cpp`
    (`FACT_DUTY_HELD`)
  * `src/rp2350/core1/emit/follower.cpp` (the deadline hold, the wait,
    `landHeld`'s `resume_s`), `ipc/core1_facts.h`, `ipc/shared_state.h`
    (`jobPower` gains `maxOnS`, `dutyLiftS`; the wait flag)
  * `docs/wire_protocol.md` if a reported state changes
* Out: the web sim.
* Depends on: ZC1.
* Status: Done (merged c659b4f). The last branch of this plan.
* Outcome:
  * Deviations: the count adds each stretch whole at its START or BREAK,
    `nextBreakDist` from rest to rest at the BEZIER's speed and accel
    (`pathOf`: `min(XY accel) × 0.707`), not `length / cutFeed`; on the bench
    a 150 mm edge took 2.9 s, not 1.9 s, and the corners were missed. A
    contour without `nextBreakDist` counts by record at the cut feed. The
    corner test uses the same time. `insertFront` is
    `beginFront(pos)`/`endFront()` around ordinary pushes; the release is
    `Executor::release()`, and `resume()` is now release + replan + commit.
    A deadline insert leaves Core 0's count as it was, so it errs high.
  * Bench (laser standing in, `maxOnS` 6, `minOnS` 3, `dwellS` 2; driver in
    `../bench-midpath`, outside the repo): a 150 mm square lifts at each
    corner with its dwell and never at the deadline; a 629 mm circle holds
    mid-arc, dwells and plunges back within 0.06 mm; on-time stays under
    `maxOnS`. A pause in a deadline dwell resumes 2 mm back in the held
    record; a pause in a corner lift resumes at the corner; an abort in a
    deadline dwell is clean. Not run: the deadline's `F_TOOL` (a clearance
    past Z's soft range fails the job's first lift first), and a passive tool
    or one without `dutyLimits` (gated by `hasDuty`).
  * Known limits: curvature slows a cut more than the count knows; the
    deadline catches it. A corner stop where Core 1's on-time is under
    `minOnS` lifts without a dwell. `queue.cpp` has no native tests.
    `test_machine` still fails to link, as on main.

## Open

* Bench: how clean a re-plunge into the slit is.
