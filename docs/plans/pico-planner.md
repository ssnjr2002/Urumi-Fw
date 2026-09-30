# Pico motion planner

First steps of moving motion planning from `web/src/toolpath` onto the Pico,
following `docs/generic_planner_firmware_design.md` (the seed). This plan covers
X and Y: straight lines, then Béziers, then the Bézier wire record. Z, A, tool
profiles and duty breaks come after, in later plans.

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
  its replan stall costs no steps. The running block's profile is the one
  exception to "Core 0 never writes what Core 1 runs": a raised exit reaches
  Core 1 as a staged piece it takes without the lock (branch 5c).
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

**Status:** done. Unblocks branch 3.

**Outcome:**

* Deviations:
  * `resume` of a held planner job answers `err moved` if X or Y left the
    held position (e.g. a jog while paused): the followers would otherwise
    jump at full step rate to the executor's position.
  * `line` is accepted in IDLE or while planner motion runs. The ring starts
    from `machinePos`, so it cannot be queued behind other motion.
  * Added `src/rp2350/core1/cycles.h` (`cycleCount()`, DWT CYCCNT, a drop-in
    for `rp2040.getCycleCount()` that is inline and interrupt-free) and put
    `RS485Bus::writeStream` in RAM. The slot loop makes no flash calls; only
    `hold`/`abort`/`resume` and the resets stay in flash.
* Interfaces for branch 3: `plannerQueueLine(x, y, feed)` → `PlannerQueueResult`
  (`src/rp2350/core0/planner/queue.h`), `plannerQueueDepth()`; `PQ_FULL`
  means retry as the ring drains. Junction deviation is `kDeviation` = 0.02 mm
  in `queue.cpp`. `RUNNING_PLANNER` = 3; `bufCount` / `buf=` count planner
  blocks (of 64) while a planner job runs or is held.
* Bench (X/Y at 160 steps/mm): single, diagonal and square lines land on their
  step targets, nodes agree; feed 30 mm/s measured 30.9 mm/s; pause holds and
  keeps the ring, resume finishes on target; cancel, abort (`0xA9`) and `stop`
  behave as planned. Open human checks: the 50 kHz stream on a scope; a zero
  `maxFeed`/`maxAccel` refusal and MicroSegment-after-planner were not run.
* Out of scope:
  * `invert` is not applied to planner motion.
  * `queuedUs` does not include planner time.
  * The MicroSegment and debug-step emitters still use `getCycleCount()`;
    `cycleCount()` drops in.
  * Channel-1 requests wait until planner motion stops, as for MicroSegment.
  * `test_parity` fails in a fresh worktree: its `*_ref.txt` vectors are
    generated from `web/`, not tracked.
  * After the `stop` test, `nodestat 2` showed `slot 1` while node 1 showed
    `slot none`; main's estop-sweep quiesce fix (`a5e8d80`) likely covers it.

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

**Status:** deferred (after branch 6). Open: jog while PAUSED needs the job's
ring kept while the jog runs (second ring or set-aside); command shape
(`jog` / `jogstop`, relative on the Pico, `RUNNING_JOG`); deadman keepalive
for continuous jog.

**Outcome:**

## Branch 4: `feature/planner-bezier`

**Plan**

* Type: feature.
* Purpose: a cubic Bézier block in `lib/planner`, X and Y only, next to the
  line block. Platform-free, contract-tested on the host, like branch 1.
* Files:
  * `lib/planner/planner/bezier.h` (new): the block data (p0-p3, `length`,
    `κ_max`, `dκ_max`, `κ_start`, `κ_end`, `t(s)` c1-c3, seed §2, §6) and
    evaluation: `t = clamp01(poly(ts, s))`, `B(t)`, `B'(t)` (seed §12).
  * An analysis helper: arc length (Gauss-Legendre), `κ` extrema, `dκ_max`,
    end curvatures, and a least-squares fit of `t(s)`. The host does this
    later (branch 6); until then tests and the bench command use this one.
    Refuses degenerate input (control point on an endpoint, cusp,
    inflection inside the piece); splitting stays a host job.
  * `planner.h`: a tagged block (line or Bézier); `push` gains a Bézier
    overload. The executor evaluates by type. Trapezoids and look-ahead are
    unchanged: both work on `length`.
  * Speed cap without A: `v_cap = min(feed, √(a_xy / κ_max))` (seed §7). The
    A terms (`ω_max`, `α_max`, `dκ`) wait for A.
  * Junctions: junction deviation on the end tangents for every join, lines
    and Béziers alike. A tangent-continuous join then costs nothing; the
    neighbours' caps limit it. The seed's Δκ limit waits for A.
  * `test/test_planner/`: Bézier contract tests (evaluator hits p0/p3
    exactly, `t(s)` monotonic and ≈ 1 at `length`, a line written as a
    Bézier with handles at 1/3 and 2/3 plans the same as the line block,
    centripetal `v²·κ ≤ a` along a quarter circle, hold stops within
    `v²/2a` inside a curve).
* Depends on: branch 2.
* Overlap: none outside `lib/planner` and `test/test_planner`.
* Checks: `pio test -e native`.

**Status:** done (merged at `a4d3f2e`). Unblocks branch 5a.

**Outcome:**

* Interface: `Planner::pushBezier(bezier, feed, limits, deviation)` next to
  the line `push`; `analyzeBezier(p0, p1, p2, p3, out)` returns
  `BezierError::{None, DegenerateHandle, Cusp, NonMonotonic}`. A block holds a
  `Path` (what look-ahead reads) plus its geometry; a resume trim is an offset
  `s0`, so both kinds trim alike.
* Deviations: inflections are allowed (they matter only for A). Planned motion
  checks the vector acceleration stays within the inscribed circle, the √2
  split holding.
* RAM: `pio run -e pico` 73,144 → 78,560 B (+5.4 KB); every ring slot carries
  a Bézier. Branch 5a revisits ring size.
* Follow-ups:
  * Rename `push` to `pushLine` (lib, tests, `core0/planner/queue.cpp`),
    after branch 5.
  * Find out what the firmware's other ~73 KB of RAM is (map file), after
    branch 5.

## Branch 5a: `bench/bezier`

**Plan**

* Type: bench.
* Purpose: measure the Bézier path on the RP2350 before firmware uses it,
  like branch 2a. Built on `bench/planner` (rebased onto branch 4).
* Measures:
  * `executor.tick()` with Bézier evaluation, from RAM: typical, at a block
    change, worst case. Budget: well inside one 1 ms tick without disturbing
    the 20 µs slots.
  * `push` + `replan()` cost at realistic Bézier block lengths (dense short
    blocks, e.g. 0.2-2 mm), and whether Core 0 keeps up.
  * The items branch 2a left open: the reverse pass's early stop and
    batching (push everything pending, replan once).
  * The analysis helper's cost on the Pico (only the bench command uses it).
  * RAM: ring size with the larger block.
* Depends on: branch 4.
* Checks: `pio run -e pico_plannerbench`.

**Status:** done (branch `bench/bezier`, not merged). Unblocks branch 5.

**Outcome:**

* Env `pico_plannerbench` now inherits `env:pico`'s flags: it had been
  dropping `PLANNER_RAM`, so branch 2a's tick numbers were from flash. The
  bench's slot loop runs from RAM too; spin-lock phases dropped (branch 2
  chose try-lock).
* `sqrtf` is newlib's software routine unless GCC may skip `errno`
  (`-fno-math-errno`); then it is one `vsqrt.f32`. The Pico SDK wraps
  `sinf`, `cosf` etc. but leaves `sqrtf` to the compiler. `env:pico` lacks
  the flag, so the firmware pays this today (branch 5 adds it).
* Results, 150 MHz, 64-block ring, try-lock, 10 s per phase:

  | | default flags | `-fno-math-errno` |
  |---|---|---|
  | `replan()`, 64 blocks | 1.04 ms | 0.11 ms |
  | `analyzeBezier` | 3.79 ms | 0.63 ms |
  | tick, line / Bézier | 1.24 / 1.93 µs | 1.17 / 1.79 µs |
  | `commit()` lock hold, 64 blocks | 19.2 µs | 19.2 µs |

  With the flag: Bézier tick under the lock at most 3.7 µs; no late slot in
  2 M; lock deferral at most 1 slot; dense 0.2-1 mm Béziers (~350 blocks/s)
  keep the ring full with no starved tick. Without it the dense ring sat at
  13-20 blocks.
* Decisions for branch 5: no early stop and no batching; per-push replan
  keeps up, and batching measured no different. Core 0's limit is
  `analyzeBezier` (~1,600 curves/s), which goes away when the host analyses
  (branch 6). Ring stays at 64: `Block` is 168 B, `Planner` 13.9 KB.
* Ring depth in distance: braking at 300 mm/s on curves (a/√2 of 2000) is
  ~32 mm, 64 × 0.6 mm is ~38 mm. At 500 mm/s it is ~88 mm, so dense short
  curves would cap feed below target (safely). Revisit with real jobs.

## Branch 5c: `fix/planner-running-exit`

**Plan**

* Type: fix (branch 2 behaviour, lines and Béziers alike).
* Purpose: a block pushed while another runs can still raise the running
  block's exit, so the first move from rest, and a stream that briefly runs
  dry, no longer stop at a block end.
* Problem: Core 1 claims a block the moment it is queued, and `claim()` pins
  its committed exit, 0 while nothing follows. The next push, about 1 ms later,
  can only plan from rest. Found on the branch 5 bench: a four-Bézier circle
  ran ¼, stopped, ran ¾; three collinear 20 mm lines took 0.30 s (one stop,
  v/a) longer than one 60 mm line.
* Prior art:
  * Marlin and Klipper never modify a running block; they delay the first
    move (Marlin `BLOCK_DELAY_FOR_1ST_MOVE`, 100 ms or 3 blocks; Klipper
    `BUFFER_TIME_START`, 250 ms). A stream that runs dry still stops.
  * grbl replans the running block: `planner_recalculate` calls
    `st_update_plan_block_parameters`, which sets its entry to
    `prep.current_speed` over its remaining millimetres, and `st_prep_buffer`
    rebuilds the rest of the block from there, up or down. It is safe because
    that speed is at the prep horizon, the end of the segments already
    buffered, which the stepper ISR cannot pass. We take this design; the
    horizon is a time Core 1 is checked not to have reached, since Core 1
    evaluates the profile directly and has no segment buffer.
* Design:
  * Only the running block's profile changes. Its geometry (path, Bézier,
    length, `s0`) is fixed once pushed, as for every block.
  * Two profiles. The ring block holds Core 0's latest offer. The executor
    copies the profile at claim and runs only its copy, so Core 1 never reads
    a profile Core 0 writes, and ticks stay lock-free between blocks.
  * One clock per block. `t` counts from the claim and is never reset; a
    rebuilt profile starts at `t_h` on that clock. Core 1 publishes `t` after
    each tick (one word, single writer).
  * Pieces. The executor runs at most two: the current profile, and one
    pending piece `{t_h, s_h, profile}` that takes over at `t_h`; from there
    `s = s_h + piece.position(t − t_h)`. It also keeps `t_end`, the block's end
    on the clock: `duration()` at claim, `t_h + piece.duration()` once a piece
    is adopted.
  * Horizon. At commit, Core 0 compares Core 1's `t` with a pending `t_h`:
    1. `t + tick < t_h`: rebuild the pending piece from the same
       `(s_h, v_h)` and replace it.
    2. `t_h − tick ≤ t < t_h`: refuse and retry; within a tick Core 1 has
       switched.
    3. `t ≥ t_h`, or nothing pending: fresh horizon `t_h = t + H`, with
       `(s_h, v_h)` evaluated on the piece now running.
    Replan treats the rest of the running block as a block of `length − s_h`
    entering at `v_h²`; the forward pass caps its exit at
    `v_h² + 2a·(length − s_h)`. `H` (a few ms) trades case 2 refusals against
    how early a raise lands; correctness does not depend on it.
  * Offer. If the new exit differs from the committed one, Core 0 builds the
    piece from `(s_h, v_h)` to the new exit, writes it into a staging slot in
    `Planner`, then sets a flag (barrier between). The rest of the ring commits
    as today, entering at the new exit.
  * Tick. Mid-block ticks run without the lock exactly every 50 slots; a tick
    is only deferred at a block boundary, where the claim or release refuses
    the commit by epoch anyway. So `tick` in the check is 1 ms plus a small
    margin, and the check runs under `plannerLock` (interrupts off, bounded).
    A slow Core 0 replan costs a retry, never a broken promise.
  * Adoption, `Executor::adopt()`, called by the follower before `needsRing`
    every tick without the lock: flag set → barrier, copy the staged piece,
    update `t_end`, clear the flag. It must precede `needsRing`: a raised exit
    can end the block sooner, and `needsRing` (running: `t + dt ≥ t_end`) must
    see that end to take the lock for the release. The check guarantees
    `t < t_h` at adoption; a flag found set at `t ≥ t_h` is a bug and raises
    an alarm rather than a speed jump.
  * Termination. Pushes come only from Core 0's command handler, so none
    arrive during the retry loop. Case 2 lasts at most a tick, then case 3
    applies. Once the remaining distance cannot use more speed, the new exit
    equals the committed one, nothing is staged and the commit succeeds. A
    claim or release meanwhile refuses by epoch, as today.
  * Hold, resume, abort: a hold brakes from the executor's own `s`, `v` and
    ignores a pending piece; a commit racing a pause stages a piece the hold
    ignores, and `restartFrom` replans from rest. `restartFrom` and `reset`
    clear the staging slot and the pending piece.
* Files:
  * `lib/planner/planner.*`: horizon replan of the claimed block, staging
    slot and flag, the three-case commit check; the header invariants
    rewritten.
  * `lib/planner/executor.*`: own profile copy, pending piece, `t_end`, `t`
    published as a volatile word, `adopt()`. `needsRing` keeps its meaning,
    reading `t_end`.
  * `src/rp2350/core0/planner/queue.cpp`: the retry loop covers horizon
    refusals. `src/rp2350/core1/emit/follower.cpp`: `adopt()` before
    `needsRing`; barriers on the RP2350.
  * `test/test_planner`: "claimed block is untouched and pins the next entry"
    becomes "claimed block's exit rises; the next entry follows" (fails before
    the fix); raise during accel and cruise (position and speed continuous
    across the switch); replace a pending piece (case 1); refuse then accept
    (case 2 → 3); raise late in braking (exit capped by the remaining
    distance, next block entering there); no offer once nothing is gained;
    `needsRing` true when an adopted piece ends the block within `dt`; hold
    with a piece pending; the first move from rest does not stop.
  * Docs: the Decisions bullet "Locking" (the claimed block's profile may be
    replaced through the staging slot), `planner.h` header.
* Depends on: branch 2. Branch 5 rebases on it.
* Checks: `pio test -e native`, `pio run -e pico`. Human: three collinear
  20 mm lines take the time of one 60 mm line; the four-Bézier circle runs
  without a stop; lines typed one by one while moving join without a stop when
  sent before the running one brakes.
* Bench, after implementing (no separate bench branch: nothing here can
  change the design; Core 1 gains a flag check and a word write per tick):
  * Counters in bench builds: offers, adoptions, refused commits, largest
    tick `dt`.
  * Rerun the branch 5a harness (`urumi-bench-bezier`, lines, bez-real,
    bez-dense, batched) against this branch's `lib/planner`: tick max, late
    slots and commit hold against the 5a numbers.
  * Sweep `H` (1, 2, 5 ms) on bez-dense and hand-typed lines; choose `H` from
    the refusal rate.

**Status:** done, awaiting merge.

**Outcome:**

* Hardware (X/Y, 160 steps/mm, feed 30): one 60 mm line 2.306 s; three
  20 mm lines back to back 2.306 s, 150 ms apart 2.309 s (before: 3.12 s,
  one stop each); 32-gon R20 lap 5.20 s, closed exactly. No PAUSED.
* Bench on `bench/running-exit` (off this branch, never merged; 5a harness cherry-picked,
  plus trickle phases that push only when the ring is down to the running
  block): replan 0.11 ms and commit hold 19.5 µs (max 23.4) at 64 blocks;
  tick max 2.6 µs unlocked, 5.05 µs under the lock (5a: 3.7; `claim()` now
  copies the profile). No late slot in any phase; deferral at most 1 slot.
* Trickle, H 2 / 3 / 5 ms: every offer adopted (lines 48-51, bez-real
  61-70), 0 late, 0 refused, 0 stops mid-run. Full-ring phases: 0-1 offers,
  3 case-2 waits.
* `H` stays 5 ms: the sweep cannot separate 2-5 ms, and 5 ms leaves the most
  slack for a Core 0 delayed by USB. H 1 ms is impossible: it is below the
  1.2 ms guard, so every offer is refused.
* Also fixed: `replan()` read the ring before recording `plan_epoch_`, so a
  claim during replan could slip past commit's check.
* Known: `test_parity` fails (on `main` too, discretize; here also missing
  fixtures).

## Branch 5: `feature/pico-bezier`

**Plan**

* Type: feature.
* Purpose: Bézier motion on the Pico: Core 0 queues Bézier blocks into the
  ring, Core 1 streams them through the branch 2 followers.
* Files:
  * `src/rp2350/core0/planner/queue.*`: `plannerQueueBezier(p1, p2, p3,
    feed)`, p0 = the previous block's end; same state, config and limit rules
    as `plannerQueueLine`. Per-push replan, 64-block ring (branch 5a).
  * `platformio.ini` `[env:pico]`: `-fno-math-errno` for the hardware square
    root (branch 5a); first check nothing in `src/rp2350` or `lib/` reads
    `errno` after a maths call. The disassembly shows `vsqrt.f32` in
    `makeTrapezoid` and no call to newlib's `sqrtf`.
  * `src/rp2350/core0/cmd/axis.cpp` / `table.h`: a debug
    `bez p1x p1y p2x p2y p3x p3y feed` command, bench only like `line`,
    analysing on the Pico. `line` and `bez` answer `err unconfigured` like the
    other controller commands (today `line` says `err no_config`).
  * Core 1: nothing new expected beyond what branch 4 puts in the executor.
  * Docs: `docs/wire_protocol.md` (the debug command).
* Depends on: branch 4, branch 5a, branch 5c.
* Overlap: `src/rp2350/core0/cmd/table.h`, `src/rp2350/core0/planner/`.
* Checks: `pio run -e pico`, `pio test -e native`. Human: a quarter circle and
  a full circle (four Béziers) close on their start step count; feed on a large
  arc matches the request; a tight arc slows to its centripetal cap; line →
  arc → line runs without a stop at tangent joins; hold, resume, cancel and
  abort mid-curve.

**Status:** done.

**Outcome:**

* First hardware run showed a pause at every quarter of a circle: a block
  pushed while another ran could not raise the running block's exit. Fixed in
  branch 5c; this branch rebased on it (one comment conflict in `queue.cpp`).
* Hardware (X/Y, 160 steps/mm, `maxAccel` 100), after the rebase:

  | check | result |
  |---|---|
  | circle R20, four Béziers, feed 30 | 4.61 s, slowest mid-lap 20.2 mm/s, closed on its start step |
  | same, feed 80 | 3.88 s, peak 43 mm/s (centripetal cap), closed |
  | arc R3, feed 80 | 0.85 s at ~14.5 mm/s, the cap; exact end |
  | line → arc R10 → line | 2.23 s, slowest 25.4 mm/s, no stop at the joins |
  | pause, resume mid-circle | PAUSED 0.43 s after the request, resumed, lap closed |
  | pause, cancel | IDLE at the hold point |
  | stop mid-circle | ESTOP → ALARM, drives off; `unstop` recovers |

* `stop` zeroes the position model (un-homed), by design. A `bez` whose start
  is not the previous end answers `err bad_curve` as intended.
* Known: `test_parity` fails as on `main`.

## Branch 6: `BEZIER` wire record and host stage 4

Planned when branch 5 is done. Scope: the `BEZIER` wire record (56 B: flags
`START` / `BREAK` / `END`, p0-p3, `length`, `κ_max`, `dκ_max`, `t(s)` `c2`,
`c3`; `κ_start`/`κ_end` and `c1` derived on the Pico), `TOOL` record, job
header with per-tool bounding boxes. Host stage 4 (split and annotate)
replaces `flatten`; `repair.ts` fixes (degenerate handles, cusp → `BREAK`).
The Pico's analysis helper then serves checks only.

## Later (not planned here)

* Z and A, blade offset, tool profiles (swivel band, overcut), duty breaks
  (reset at lifts, forced break otherwise), mesh.
* Retire the MicroSegment job path and `lib/motion`.
