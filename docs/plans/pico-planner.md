# Pico motion planner

First steps of moving motion planning from `web/src/toolpath` onto the Pico,
following `docs/generic_planner_firmware_design.md` (the seed). This plan covers
X and Y and straight lines only, entirely on the Pico. Béziers, the host side,
Z, A, tool profiles and duty breaks come after, in later plans.

## Decisions

* **Seed design, not a port.** The streaming Bézier planner of the seed replaces
  the sample-space pipeline. `lib/motion` (the C++ port of `web/src/toolpath`)
  is deleted once nothing depends on it; not in this plan.
* **Lines first.** A line is a block type next to the Bézier the seed describes.
  The Pico generates lines itself; jog needs them, so they are not throwaway.
* **Planner on Core 0, tick on Core 1** (seed §4). Core 0 plans: speed caps,
  junctions, look-ahead, trapezoids. Core 1 evaluates: a 1 kHz tick turns time
  into position, followers turn position into steps.
* **Fixed-rate stream at 50 kHz.** One stream byte per step-routine tick, steps
  or not. At `RS485_BAUD` 921600 with 11-bit frames that is ~60 % of the bus,
  and 50 k steps/s per axis (312 mm/s at 160 steps/mm). The node already shapes
  the pulse (3 µs one-shot, `src/node/board/attiny3224/stepper/stepper.h:22`),
  so the cap is one step per byte per axis, not the seed's half step.
* **Position followers** (seed §13): Q32.32 accumulators per axis. The integer
  part is the step position; Core 1 publishes it to `machinePos[slot]` every
  tick, and seeds the accumulators from `machinePos` at motion start. Single
  writer and meaning unchanged (`src/rp2350/ipc/shared_state.h:238-243`).
* **Line junctions use junction deviation** (GRBL). The seed's Δκ rule gives
  nothing for line-to-line corners.
* **No Pico command for lines in the interface.** Core 0 has an internal API
  (`plannerQueueLine`); jog calls it. A debug command exposes it for bench
  testing only, like the step-debug burst (`src/rp2350/core0/cmd/axis.cpp:453`).
* **Coexistence.** MicroSegment jobs and planner motion are two Core 1 modes
  that never overlap; nothing but the planner API reaches the new path.
* **Rejected: node-side interpolation** (per-tick increments, node runs its own
  accumulator). It needs a shared time base across nodes, which was tried early
  on over the half-duplex bus without an acceptable solution.

## Branch 1: `feature/planner-core`

**Plan**

* Type: feature.
* Purpose: the planner as a pure library: line blocks, per-block limits,
  junction deviation, look-ahead (seed §10), trapezoids, and the evaluator that
  turns (block, time) into an XY position in mm. Float only, no Arduino or Pico
  headers, contract-tested on the host.
* Files:
  * New `lib/planner/` (headers under `lib/planner/planner/`):
    * block type and ring (fixed capacity, no heap), single-threaded. Look-ahead
      is split so branch 2 can lock around the short half (seed §15):
      `replan()` computes speeds into scratch without touching the ring,
      `commit()` writes them and refuses if a block was claimed since. `claim()`
      records the executing block's exit speed, which pins the next block's
      entry (seed §10). Barriers and the spinlock live in branch 2.
    * limits from per-axis `maxFeed` / `maxAccel` projected onto the line
      direction; junction deviation at line corners, the deviation a parameter
      (branch 2 passes a firmware constant). No input validation: limits are
      trusted, validation comes later at the config level.
    * look-ahead reverse/forward passes; last queued block exits at 0.
    * trapezoid build and closed-form `s(t)` (seed §12).
    * feed hold: from the current `(s, v)`, decelerate along the same block
      and following blocks to rest; abort is a hold that discards the ring.
  * New `test/test_planner/`: contract tests (feasibility of every trapezoid,
    junction limits respected, exits at rest, hold stops within `v²/2a`,
    evaluator hits block endpoints exactly).
  * `platformio.ini` `[env:native]` (`:436`): add `test_planner` to
    `test_filter`.
* Depends on: nothing. Touches no controller code; can run in parallel with
  other work.
* Overlap: `platformio.ini`.
* Checks: `pio test -e native`.

**Status:** in progress.

**Outcome:**

## Branch 2: `feature/pico-follower`

**Plan**

* Type: feature.
* Purpose: run `lib/planner` on the Pico for X and Y: Core 0 plans lines into
  the ring, Core 1 streams them at 50 kHz through position followers.
* Files:
  * `platformio.ini` `[env:pico]`: link `lib/planner`.
  * New `src/rp2350/core0/planner/`: `plannerQueueLine(x, y, feed)`, limits
    from `MachineCfg` (`src/rp2350/core0/config/config_decode.h:30-45`:
    `stepsPerUnit`, `maxFeed`, `maxAccel`); soft-limit check against
    `maxTravel` (0 = none) on homed axes, refuse on unhomed axes.
  * New `src/rp2350/core1/emit/follower.cpp`: the 1 kHz tick (claims blocks,
    evaluates position, converts to Q32.32 step targets) and the 50 kHz step
    routine (accumulators → one stream byte per tick, seed §13-14), both
    `__time_critical_func`. Publishes `machinePos`.
  * `src/rp2350/core1/core1.cpp` `processBus()` (`:45`, `:82`): a planner
    branch next to `processMicroSegments()`; estop and abort handling for it.
  * `src/rp2350/ipc/shared_state.h`: the planner ring, pause/abort request
    reuse, a new `runningReason` for planner motion.
  * `src/rp2350/core0/cmd/axis.cpp` / `table.h`: a debug `line x y feed`
    command calling `plannerQueueLine`.
  * `src/rp2350/core0/status.cpp`, `cmd/query.cpp:104`: queue depth reports
    the planner ring while it is the active mode.
  * `web/src/wire/format/status.ts`: the new `runningReason`.
  * Docs: `docs/wire_protocol.md` (the debug command, the running reason).
* Depends on: branch 1.
* Overlap: `platformio.ini`, `src/rp2350/core0/cmd/table.h`, `web/src/wire/`.
* Checks: `pio run -e pico`, `pio test -e native`, `pnpm typecheck` and
  `pnpm test` in `web/`. Human: stream rate on a scope (50 kHz, jitter); a
  single line lands on its target step count; a square of lines corners at the
  junction speed; hold mid-line stops smoothly and `getpos` matches the node
  counters; abort; estop; soft limit refuses a line past `maxTravel`;
  MicroSegment jobs still run after planner motion.

**Status:** not started.

**Outcome:**

## Branch 3: `feature/pico-jog`

**Plan**

* Type: feature.
* Purpose: jog built on planner lines inside the Pico. A step jog is one short
  line; a continuous jog is a line toward the soft limit that a stop holds.
  Replaces the host-built JOG packet burst.
* Files:
  * New jog command(s) on the control plane calling `plannerQueueLine` and the
    hold path; `src/rp2350/core0/cmd/table.h`.
  * `src/rp2350/core0/data_plane.cpp` (`:138`, `:281`): remove `JOG_MAGIC`.
  * `web/src/operatorJog/`: `makeJog.ts` (JOG packet burst) replaced by the
    new commands; `clickJogSource.ts`, `jogTo.ts` follow.
  * `web/src/wire/format/`: drop the JOG packet encoding.
  * Docs: `docs/wire_protocol.md` (JOG packet removed, jog commands added).
* Depends on: branch 2.
* Overlap: `src/rp2350/core0/cmd/table.h`, `web/src/wire/`.
* Checks: `pio run -e pico`, `pnpm typecheck` and `pnpm test` in `web/`.
  Human: step and continuous jog on X and Y from the web UI; release stops
  within the hold distance; jog refused past soft limits and when unhomed where
  that applies; jog during pause returns to PAUSED.

**Status:** not started.

**Outcome:**

## Later (not planned here)

* Bézier block type and the `BEZIER` wire record (56 B: flags `START` /
  `BREAK` / `END`, p0-p3, `length`, `κ_max`, `dκ_max`, `t(s)` `c2`, `c3`;
  `κ_start`/`κ_end` and `c1` derived on the Pico), `TOOL` record, job header
  with per-tool bounding boxes. Host stage 4 (split and annotate) replaces
  `flatten`; `repair.ts` fixes (degenerate handles, cusp → `BREAK`).
* Z and A, blade offset, tool profiles (swivel band, overcut), duty breaks
  (reset at lifts, forced break otherwise), mesh.
* Retire the MicroSegment job path and `lib/motion`.
