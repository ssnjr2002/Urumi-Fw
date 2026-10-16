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
| duty, past `maxOnS` | lift, tool off, `DWELL`, tool on, plunge | anywhere | ZC |
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
  at the same XY, continue the rest of the block from `s`. Putting the lift
  and plunge between the held point and the rest of the block is new planner
  work (planner branch 2's `restartFrom` resumes in place). A duty lift due
  shortly is moved to an earlier `START` that lifts anyway. Triggers are live
  counters on Core 1 that request a hold early enough to stop in time.

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
  `NODE_TYPE_KNIFE_OSC` → `CMD_KNIFE_OSC`). The tool-level `dutyLimits` is
  refused by name (as `clearanceMm` was). Blob and schema version 9.
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
    at rest, then alarms; a failed `TOOL_ON` leaves the head at clearance,
    unplunged.
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
    `ipc/shared_state.h` (`JOB_DWELL`)
  * `config/controller.jsonc:211`, fixtures and tests,
    `docs/tool_duty_limits.md` (marked superseded by this doc; its host
    scheduler is already gone from `web/src`), `docs/wire_protocol.md`
* Out: the blower, the alarm lift ("Every other alarm lifts"), the lost-host
  lift.
* Depends on: ZA.
* Status: Planned.

### ZC: mid-path duty lifts

* Name: `feature/job-midpath-lifts`
* Purpose: hold, lift, `DWELL` and plunge back mid-block when on-time passes
  `maxOnS`; continue from `s`.
* Files: `lib/planner/` (split a held block around a lift).
* Depends on: ZB.
* Status: Planned.

## Open

* Bench: how clean a re-plunge into the slit is.
