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
* **Locking: the caller locks, core 1 only at block changes.** `lib/planner`
  stays platform-free and single-threaded; its two callers (`plannerQueueLine`
  on Core 0, the follower on Core 1) hold one hardware spinlock around ring
  edits: `push()` and `commit()` on Core 0; on Core 1 only ticks that claim or
  release a block (`Executor::needsRing(dt)`), and `resume()` / `abort()`.
  `replan()` runs unlocked. Resume runs on Core 1: the machine is at rest, so
  its replan stall costs no steps.
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

**Status:** done (merged at `32d7248`). Unblocks branch 2.

**Outcome:**

* Interface for branch 2 (`planner/planner.h`, `planner/executor.h`):
  * One shared `Planner`. Lock around `push()`, `commit()`, `claim()` and
    `release()`; `replan()` runs unlocked. A single epoch, bumped by every
    push, claim and release, makes a stale `commit()` refuse.
  * `Executor` runs on core 1: `tick(planner, dt)` returns XY; `hold()`,
    `resume(planner)`, `abort()`. `resume()` and `abort()` call planner methods
    that touch the ring (`restartFrom()`, `replan()`, `commit()`, `reset()`),
    so branch 2 must lock around them or route them to core 0.
  * `Planner::restartFrom(s)` added (not in the plan) for resume: trims the
    claimed block to the stop point and replans from rest.
  * `reset(pos)` on both declares the machine at rest at `pos`; call it after
    homing, probing, jogging on the old path, or anything else that moves
    outside the planner.
* Deviation stays a `push()` argument so it can later vary per tool.
* Zero-length moves are no-ops. `maxAccel = 0` is not handled (no
  validation, as agreed); branch 2 decides whether 0 means uncapped.
* Not done: the seed's early stop in the reverse pass; `replan()` always
  walks the whole ring (at most 64 blocks, outside the lock).
* `pio test -e native`: `test_parity` fails in a fresh worktree because its
  reference files (`constrain_ref.txt` etc.) are untracked and must be
  generated from `web/`. Not caused by this branch; the other suites pass.

## Branch 2a: `bench/planner`

**Plan**

* Type: bench.
* Purpose: measure `lib/planner` on the RP2350 before the follower is built,
  to choose Core 1's lock behaviour (try-and-retry-next-slot vs. spin-wait).
* Files:
  * `platformio.ini`: a scratch env linking `lib/planner`.
  * New `src/scratch/planner_bench.cpp`:
    * Core 0 loops: push random lines, `replan()`, `commit()` under the lock.
    * Core 1 runs a 20 µs cycle-counter slot loop like the MicroSegment
      emitter (`src/rp2350/core1/emit/microsegment.cpp:123`), with
      `executor.tick()` every 50 slots; it takes the lock only at block
      changes. No bus traffic needed.
    * Results over USB serial.
* Measures:
  * `executor.tick()` cost, typical and at a block change.
  * `commit()` lock hold time at 1, 16 and 64 queued blocks.
  * `replan()` cost at 64 blocks (the resume stall).
  * How often Core 1 finds the lock busy, and for how many slots.
  * Slot lateness histogram under each lock behaviour.
* Depends on: branch 1.
* Checks: `pio run -e <bench env>`.

**Status:** done (branch `bench/planner` at `2230c1e`, not merged). Unblocks
branch 2.

**Outcome:**

* Env `pico_plannerbench`, `src/scratch/planner_bench.cpp`. Timed with the DWT
  cycle counter: `rp2040.getCycleCount()` misses wraps while core 0 holds the
  lock with interrupts off.
* `Executor::needsRing(dt)` was added to `lib/planner` on the bench branch
  (`f3a770d`, `feat(planner)`, with a native test); branch 2 cherry-picks it.
* Results, RP2350 at 150 MHz, 64-block ring, code running from flash:
  * Executor tick: 1.1 µs without the lock, 2.0 µs at a block change.
    Cold-cache outliers up to 26 µs on the first ticks.
  * `commit()` lock hold: ~0.3 µs per block, 19.3 µs at 64 blocks.
  * `replan()`: ~16 µs per block, 1.0 ms at 64 blocks (the resume stall, and
    core 0's cost per push with a full ring).
  * Stress (0.5 mm lines, ~470 blocks/s): try-lock found the lock busy 98
    times in 10 s, deferring at most 2 slots, with no late slots; spin waited
    up to 19.1 µs and made 12 slots late (all < 4 µs). Commits refused: 7-8
    of ~9400.
* For branch 2: core 1 uses try-lock and retries next slot. Put the tick path
  in RAM (`__time_critical_func`) or warm the cache before motion, to remove
  the cold outliers.
* Later, out of scope:
  * The reverse pass's early stop (seed) and a commit that copies only
    changed blocks would cut both the 1 ms replan and the 19 µs lock hold.
  * Batching: push everything that has arrived, then replan once. Safe, since
    a pushed block sits at rest until the next commit. No batch size to
    choose; it settles at arrival rate × replan cost. Two constraints:
    throughput (replan cost < one block's run time; per-push replan falls
    behind below ~0.7 ms blocks, e.g. 0.2 mm at 300 mm/s) and a deadline
    (commit before the machine enters the old plan's final braking ramp;
    ~30 ms of slack in the stress case).
  * Ring depth in distance must exceed the braking distance (22.5 mm at
    300 mm/s, 2000 mm/s²) or full feed is never reached; 64 × 0.2 mm is not.
  * Bench before the Bézier work, on `bench/planner`: batching, early stop and
    dense blocks, at realistic Bézier block lengths.

## Branch 2: `feature/pico-follower`

**Plan**

* Type: feature.
* Purpose: run `lib/planner` on the Pico for X and Y: Core 0 plans lines into
  the ring, Core 1 streams them at 50 kHz through position followers.
* Files:
  * `platformio.ini` `[env:pico]`: link `lib/planner`.
  * `lib/planner`: `Executor::needsRing(dt)`, true when a tick will claim or
    release a block.
  * New `src/rp2350/core0/planner/`: `plannerQueueLine(x, y, feed)`, `feed` in
    mm/s; limits from `MachineCfg` (`src/rp2350/core0/config/config_decode.h:30-45`:
    `stepsPerUnit`, `maxFeed`, `maxAccel`). Refuses a line when `maxFeed` or
    `maxAccel` is 0 on X or Y. No homing requirement and no soft-limit check
    for now. On an empty ring it resets the planner and executor at
    `machinePos` in mm. Replan and commit retry until `commit()` succeeds.
  * New `src/rp2350/core1/emit/follower.cpp`: the 20 µs slot loop (followers →
    one stream byte per slot, seed §13-14) with the executor tick every 50
    slots, locking per the Decisions entry; the lock behaviour on a busy lock
    is whatever branch 2a's numbers chose. Publishes `machinePos`.
  * No guard between planner motion and MicroSegment jobs; the two paths stay
    independent.
  * `src/rp2350/core1/core1.cpp` `processBus()` (`:45`, `:82`): a planner
    branch next to `processMicroSegments()`; estop and abort handling for it.
  * `src/rp2350/ipc/shared_state.h`: the planner ring, pause/abort request
    reuse, a new `resumeRequested` flag, `RUNNING_PLANNER = 3` for planner
    motion. Braking to a hold or abort shows `RUNNING_ABORT_DECEL`, as the
    MicroSegment path does.
  * Pause, resume, cancel for planner motion (`cmd/lifecycle.cpp`): `pause`
    holds and keeps the ring, then PAUSED; `resume` in PAUSED with a planner
    ring sets `resumeRequested`, Core 1 calls `executor.resume()` and goes back
    to RUNNING; `cancel` in PAUSED drops the ring, then IDLE; abort holds, drops
    the ring, then IDLE. MicroSegment pause/resume is unchanged.
  * `lib/planner`: a `PLANNER_RAM` function attribute, empty by default; the
    Pico build defines it as `__not_in_flash_func` for the tick path.
  * `src/rp2350/core0/cmd/axis.cpp` / `table.h`: a debug `line x y feed`
    command calling `plannerQueueLine`. No engaged/enabled preflight for now.
  * `src/rp2350/core0/status.cpp`, `cmd/query.cpp:104`: queue depth reports
    the planner ring while it is the active mode.
  * `web/src/wire/format/status.ts`: the new `runningReason`.
  * Docs: `docs/wire_protocol.md` (the debug command, the running reason).
* Depends on: branch 1, branch 2a's Outcome.
* Overlap: `platformio.ini`, `src/rp2350/core0/cmd/table.h`, `web/src/wire/`.
* Checks: `pio run -e pico`, `pio test -e native`, `pnpm typecheck` and
  `pnpm test` in `web/`. Human: stream rate on a scope (50 kHz, jitter); a
  single line lands on its target step count; a square of lines corners at the
  junction speed; hold mid-line stops smoothly and `getpos` matches the node
  counters; abort; estop; a line is refused on a zero `maxFeed`/`maxAccel`;
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
