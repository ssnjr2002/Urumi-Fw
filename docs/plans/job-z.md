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
* With work Z the bed top (the mesh applies by its rule):

  ```
  lift / travel   workZ + t + c
  plunge          workZ + t − k·t      k in [0, 2]; 1 a full cut, past 1 into the mat
  ```

  `t` (material thickness) from the layer header, `k` (plunge scale) from the
  layer header, `c` (`clearance`) from the tool profile. `t` is a u16 in µm
  (to 65.5 mm; Z travels 48 mm, so preflight bounds it first). A tool with
  `plunge: false` (a pen) has `k` 0.
* **Feeds:** a plunge at the tool's `plungeFeed`, else
  `machine.feed.plungeFallback`; a lift at `machine.feed.liftFeed` (new,
  machine-wide, ≥ 1).

### What lifts

| cause | stop | where | branch |
|---|---|---|---|
| contour `START` | lift, travel, plunge | every contour start | ZA |
| job end | lift | after the last record | ZA |
| hold to `PAUSED`, abort at rest | lift | where it stopped | ZA |
| dry ring (`BUFFERING` mid-job, lost host) | lift | where it stopped | ZA |
| duty, past `maxOnS` | lift, tool off, `DWELL`, tool on, plunge | anywhere | ZC |
| corner over `cornerAngleDeg` | lift, swivel, plunge | that `BREAK` | with A |
| cable range | lift, unwind, plunge | anywhere | with A |

Estop does not lift. Every other alarm lifts when Z can still move, turns the
tool off and keeps the vacuum on.

* The lift at a contour's `START` separates the travel from its first cut,
  so the corner blend between them (kept by JX) goes.
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
* Purpose: Z-only lift and plunge blocks at contour starts, at job end, on
  pause, abort and a dry ring; `t` on the wire, `k` live.
* Interface: `LAYER_HDR` gains `t` (u16 µm, so 22 bytes); `machine.feed.liftFeed`;
  tool `clearance`; blob and schema version 8.
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
* Status: Planned.

### ZB: tool events and duty at START

* Name: `feature/job-tool-events`
* Purpose: tool on/off event blocks, `settleS`, the duty counter and `DWELL`
  at contour starts.
* Open: which node switches each tool, and how the config names it.
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
