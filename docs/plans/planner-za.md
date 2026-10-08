# Z and A in the planner

Z and A on the Pico planner (`lib/planner`, Core 1's follower), so that jogs,
lifts, plunges, tangential cutting and mesh levelling run on the planner and
the MicroSegment path can be retired. Follows docs/plans/pico-planner.md
(its "Later" list) and docs/plans/motion-sessions.md (jogs). Planner scope
only; the job layer (header, `TOOL` record, framing) is motion-sessions' and
uses what this plan builds.

## Decisions

### Axes

* **Free axes and derived axes.** A line moves any of X, Y, Z, A freely (jog,
  lift, plunge, rotate). While XY moves (cut or travel), Z and A are derived
  from the XY path: Z from the work Z plus the mesh, A from the path heading.
  The wire stays 2D: the host sends XY geometry only.
* **The selected head's Z and A are streamed; the other head's Z is parked by
  node park legs.** The stream byte carries 4 slots (X, Y, Z, A;
  `core1/emit/follower.cpp:18`); two heads have 6 axes. The unselected head
  only has to stay parked, which a node park leg does on its own, both heads
  in parallel (motion-sessions J1b). A head switch lifts the old head through
  the planner while it is still selected, then `select` rebinds slots 2 and 3.
* **No 3D cutting.** Z is never a free axis during an XY move; the mesh is the
  only Z under XY motion.
* **Moves on the same set of axes join by junction deviation;** on a single
  axis (Z or A) that is full speed straight on and a stop on a reversal. A
  change of axis set (XY to Z, Z to A, …) stops: there is no angle between
  mm and degrees, and a knife stops there anyway. Z and A step-jog clicks
  then join like XY ones.

### A

* **No machine range on A.** A's position is unbounded. A tool can limit
  how far A may rotate (the knife's cable); the job passes that tool's range
  to the planner.
* **Wraps are an integer count.** A is a bounded heading plus an integer
  wrap count k of the tool's period P = 360°/n. The step target is computed
  absolutely, round((heading + k·P) · steps/deg), in Q32.32 like the
  followers; never shifted by a rounded amount. A period that is not a whole
  number of steps then costs at most half a step per re-pick, without
  accumulating, so no tool is refused for it. No float grows on a long spiral.
* **Heading choice at lifts.** At a lift (contour start, a corner the tool
  lifts at) any equivalent mod P may be chosen. Mid-cut A follows the path.
* **Cable range.** The planner picks the start equivalent that keeps a sweep
  inside the range, and answers whether a stretch's sweep fits. The sweeps
  come from the job: XY-only geometry the host computes from tangents and
  corner turns, sent with every `START` and `BREAK` as two intervals relative
  to the heading there: to the next `BREAK`/`END` (stretch) and to `END`
  (rest of contour). The Pico lifts at a lift point when the rest fits,
  otherwise checks each stretch before its `BREAK` and lifts there when the
  next stretch would leave the range; a single stretch wider than the range
  is refused at ingest. The wire change is the job's, not this plan's.
* **A limits XY speed** (from the seed session):
  * turn rate: v ≤ ω_max / κ_max;
  * turn acceleration: α = a_t·κ + v²·dκ/ds, so v ≤ √(α_frac·α_max / dκ_max)
    and a reduced tangential acceleration per block;
  * a junction limit where curvature jumps between blocks.
  * Units: A's `maxFeed`/`maxAccel` are deg/s, deg/s²; converted to rad
    against κ in 1/mm.

### Blade offset

* **One 2-vector per tool, in the knife's frame:** `d` along the cut (the
  trail) and `e` across it (misalignment, normally 0). It is the tool tip
  offset of coordinate-system.md. The knife axis follows `P + d·T + e·N`.
  Separate from the head offset (fixed, not rotating, already applied by
  `frames`).
* **XY limits are checked at the axis point,** which moves up to
  √(1 + (r·κ)²) faster than the tip on curves, r = √(d² + e²).
* **Reach:** the axis path lies inside the tip bbox grown by r, whatever the
  heading (swivels included). Preflight and the per-record hull check grow by
  r; that is the job's. The planner plans the axis path and needs nothing
  more for reach.

### Mesh

* **Applied to every move, jogs included,** after the executor and before
  the followers (Marlin and Klipper `bed_mesh` likewise; no fade), once X, Y
  and Z are homed. Lift and plunge are pure Z lines to (lift height or work
  Z) + mesh at that point.
* **One mesh of the bed, looked up at the selected tip** (anchor + head
  offset), so both heads see the same surface. Probing fills it head by
  head where each reaches; a constant difference between heads is their
  work Z.
* **Relative to the work origin:** the offset is mesh(tip XY) − mesh(work
  origin XY), a height difference (+ up, as Z). A head's work Z is taught by
  a touch that already includes the bed there.
* **The planner's Z is flat; the mesh is a rule, not a state.** Whenever the
  mesh is active (loaded; X, Y, Z homed): actual Z = flat Z + offset(tip).
  * Core 1, every tick: Z target = the executor's flat Z + offset(tip now).
  * Core 0, whenever the ring starts from the motors (`posFromSteps`: the
    first push after a drain or idle, homing included, and a jog start):
    flat Z = actual Z − offset(tip now). The first tick then targets where
    Z already is, so Z never jumps; it follows the change in offset from
    there.
  * A hold, resume or abort keeps the executor's flat position; nothing is
    recomputed.
  * The offset changing at rest (mesh loaded, homing done, `select`, the
    work origin moved) moves nothing: the next restart absorbs it.
* **Work Z is stored flat:** `wzero`/`wset` Z store actual − offset(tip), so
  a Z zeroed away from the work origin is not corrected twice.
* **Positions:** `MPos` Z is actual (the motors). `WPos` Z = actual −
  offset(tip) − work Z, computed on Core 0 from `machinePos` (`frames`);
  0 is on the material everywhere. `STATUS_RSP` stays steps.
* **Grid in RAM, read by Core 1 every tick:** a bilinear lookup in RAM-only
  code (no libm or library calls; casts instead of `floorf`), about 100
  cycles of the tick's 3000-cycle slot. Core 0 walks each block at push, at
  about half the grid spacing, for its slope and its Z range.
* **Upgrade if grids outgrow RAM: a point ring.** Core 0 keeps the grid in
  flash and, at push, samples each block's offset every Δs (half the grid
  spacing) into a ring of int16 values beside the block ring; Core 1 reads
  point s/Δs and its neighbour, and never sees the grid.
  * Blocks are pushed and released in order, so the points are a FIFO:
    appended at push, freed at release, cleared with the block ring on an
    abort or idle reset. No allocator.
  * A full point ring is a full ring (`PQ_FULL`). 4096 points (8 KB) hold
    about 20 m of path at Δs 5 mm; look-ahead needs a stopping distance.
  * Points are keyed by distance along the block's whole geometry, so a
    block trimmed on resume (`s0`) needs nothing.
  * RAM stays flat however fine the grid. Only Core 1's source of the
    offset changes; the push-time walk is the same.
* **Z limits XY speed** too: v ≤ Z `maxFeed` / |slope| along the block, and
  Z's acceleration from the change of slope (block boundaries included).
  A load-time check reports the worst case; on this bed it should never bind.
* **Soft range per block:** flat Z plus the mesh's range along the block
  must stay inside Z's soft range; refused as other soft-limit moves.
* **Storage: `/mesh.bin` on LittleFS, apart from the config.** Fixed layout,
  so a probe can rewrite one point in place:

  ```
  magic, version, nx, ny, x0, y0, dx, dy   (machine frame, mm)
  int16 z[nx*ny]                            (µm, row-major)
  crc32
  ```

  Any nx × ny up to 16384 points (32 KB). Bilinear between points, the edge
  value outside the grid. Missing or a bad CRC means flat, reported by `get`.
  Written only at rest (no flash writes during motion). Filling it by probing
  (the BLTouch head) is a later plan.

## Branches

A dependent chain, one session: 1, 2, 4, then 3 → 3b. Between 4 and 3,
motion-sessions' job branch inserts lifts and plunges (contour start, travel,
corners past `cornerAngleDeg`, duty breaks), so a tangential cut can be
tested.

## Branch 1: `refactor/planner-axes`

### Plan

* Type: refactor.
* Purpose: `lib/planner` positions and limits from 2 axes to 4, XY behaviour
  unchanged; existing tests pass as they are.
* What the code has today:
  * `Vec2` and `AxisLimits` (`max_feed[2]`, `max_accel[2]`) in
    `lib/planner/planner/path.h:15`, `:20`.
  * `Line` (`planner/line.h:16`), `Bezier::p` (`planner/bezier.h:22`),
    `Block::origin` (`planner/planner.h:52`), `Planner::reset`/`pushLine`/
    `end` (`planner.h:73`, `:79`, `:119`), `Executor::reset`/`tick`/
    `position` (`planner/executor.h:33`, `:41`, `:58`) are all `Vec2`.
  * `makeLine` caps per axis over 2 (`line.cpp:23`); `pathOf` takes
    `min(max_feed[0], max_feed[1])` (`bezier.cpp:194`).
* Scope:
  1. A position type for block ends (`Block::origin`, `Path::end`), the
     executor's output and `reset`/`end`:
     `struct Pos { float x, y, z; float a; Vec2 xy() const; void setXy(Vec2); }`
     (mm; A in deg). Not a vector: its units are mixed and no maths runs
     across all four. `AxisLimits` over 4. XY geometry (Bézier control
     points, tangents, curvature) stays `Vec2`; Z and A pass through
     unchanged.
  2. Callers pass and take the wider types, using X and Y only:
     `core0/planner/queue.cpp` (`:27` `admit`, `:54`, `:182`),
     `core1/emit/follower.cpp:183`, `core0/cmd/lifecycle.cpp:116`,
     `core1/core1.cpp:49`, `core0/core0.cpp:140`.
  3. Tests reach XY through `.xy()` (`test_executor.cpp`, `test_bezier.cpp`);
     what they check is unchanged.
* Out of scope: any Z or A motion.
* Overlap: `lib/planner/`.
* Checks: `pio run -e pico`, `pio test -e native`.

### Status

Done.

### Outcome

* `Path::end` stays `Vec2`: `Path` is geometry (junctions read it). A
  block's Z and A live in `Block::origin`, now set for Béziers too; block
  ends take XY from `Path::end` and Z, A from `origin`.
* `admit` fills limits for X and Y only; Z and A stay 0 until branch 2.
* Found, out of scope: PlatformIO reports doctest suites that pass as
  SKIPPED, because it parses only the per-case blocks doctest prints for
  failures; a case-per-block doctest reporter in `test/main.cpp` would fix
  it. `test/main.cpp:2` still says "motion suite".

## Branch 2: `feature/planner-za`

### Plan

* Type: feature.
* Purpose: lines in Z and A (jog, lift, plunge, rotate), followed on Core 1;
  Z and A jogs move off `JOG_MAGIC`, which is retired.
* Depends on: branch 1.
* What the code has today:
  * The follower seeds and steps slots 0 and 1 only
    (`core1/emit/follower.cpp:123`, `:153`, `:195`); `plannerSpm[2]`
    (`ipc/shared_state.h:365`) is set in `queue.cpp:52`.
  * `JOG_MAGIC` (`core0/data_plane.cpp:149`, `:193`) carries Z and A through
    the MicroSegment ring; the web packs it (`web/src/wire/format/constants.ts:15`).
  * Step jogs (`core0/cmd/axis.cpp:642` `queueJog`) and continuous jogs
    (`data_plane.cpp:123`) are XY; the continuous packet reserves Z and A
    (`web/src/wire/format/cjog.ts:7`).
  * A's `stepsPerUnit` is steps per degree (45.98 in
    `config/controller-1head.jsonc:117`).
* Scope:
  1. Line blocks moving one axis set: XY (either or both), Z alone or A
     alone; a line mixing sets is refused (mm and degrees share no length).
     Junctions as in Decisions, Axes: each `Path` carries its axis set, and
     a Z or A line's direction is `{±1, 0}`, so the junction formula gives
     full speed straight on and a stop on a reversal; different sets stop.
  2. Follower: slots 2 and 3 (the selected head's Z and A), `plannerSpm[4]`,
     seeded from `machinePos`. Z and A limits and steps/unit from the head
     bound to each slot: `frames.cpp:115` `axisFor` exported.
  3. `Pos::a` becomes a heading in [0, 360) plus an integer `turns`; the step
     target is `turns · (360 · spm) + heading · spm` in Q32.32, so A's
     resolution does not fall with turns (a plain float is past a step after
     ~1000 turns). Branch 3 re-picks by whole periods within this form.
  4. Step `jog` and continuous jogs accept Z and A; `jogto` stays XY (J1b
     decides absolute Z and A). Soft range on Z through `framesCheckMove`,
     which covers slot 2; none on A. A CJOG
     packet holding more than one axis set is NACKed `MIXED_AXES` (0x0A);
     moving from one held set to another still brakes and starts anew.
     `JOG_MAGIC` refused, then removed with every web use of it
     (`web/src/operatorJog/`: `makeJog`, `clickJogSource`, `jogClick`,
     `jogTo`, and their tests); Z and A clicks become `jog z|a`.
  5. Docs: `docs/wire_protocol.md`.
* Out of scope: derived Z and A, tools.
* Overlap: `lib/planner/`, `web/src/wire/`, `src/rp2350/core0/cmd/table.h`.
* Checks: `pio run -e pico`, `pio test -e native`, `pnpm typecheck`,
  `pnpm test`. Human: Z and A step and continuous jogs on each head; a Z
  soft-limit refusal; `nodestat` deltas match the Pico.

### Status

Done, merged 2026-10-08 (`e89ca85`..`f245f09`). Unblocks branch 3.

### Outcome

* An unhomed step jog on A is capped at one turn, and a held A jog at one
  turn per hold (`CJOG_A_RUN`), for the cabled knife; homed A step jogs are
  uncapped.
* `JOG_MAGIC` is NACKed `BAD_STATE`; its 26 bytes are still read so the
  stream stays in sync.
* Web, deviating from Scope 4: `jogTo`/`jogToPoint` were ported to `jog`
  rather than removed (one `jog` per moving axis, then a wait for IDLE), so
  `jogToPoint` now runs its axes one after another, not as one coordinated
  move. Later removals delete replaced web code and leave callers broken.
  `AxisCalibration` gained `invertDir` and `jogFeed`; the host's `invert`
  and the Pico's `invertDir` disagree on Z.
* Demo: barebones jogs Z (R/F) and A (Q/E) and selects head 0, 1 or the
  anchor.
* Found, out of scope:
  * `memset` runs from flash: arduino-pico's `memmap_default.ld` keeps
    `*libc.a:*lib_a-mem*.o` in RAM, but GCC 14.3 names it
    `libc_a-memset.o`, so the pattern misses. RAM code calling it stalls on
    XIP.
  * Dead now that no jog bursts arrive: `streamIsJog`, `RUNNING_JOG` and
    the paused-jog path (`core1/emit/microsegment.cpp:159-167`, `sim.ts`).
  * Docs still describing jog bursts: `comms_architecture`,
    `coordinate_frames_and_limits`, `feed_override`, `PLAN_phase1_host_impl`.
  * `pnpm lint` fails on `main` too (`axisChars` in `sim.ts` among others).
  * Bench: with Z `softLimits` false, a held Z jog ran past the top switch;
    node 3 then reported `limit 1`. Z soft limits are now on, and Z
    `maxTravel` measured (50) on `main`.
* Checks: `pio run -e pico`, `pio test -e native` (65 cases), `pnpm
  typecheck`, `pnpm test` (1072) pass. Human: Z and A jogs on head 1; Z
  re-checked and re-homed after the overrun.

## Branch 3: `feature/planner-tangential`

### Plan

* Type: feature.
* Purpose: A follows the path heading on Béziers and XY lines when the tool
  is tangential; A's speed and acceleration limits; heading choice at lifts
  with period and cable range.
* Depends on: branch 2.
* Scope:
  1. Executor: A from the XY tangent, continuous across blocks of a stretch.
  2. Limits (Decisions, A): turn rate, turn acceleration and the reduced
     tangential acceleration per block, the curvature-jump junction limit.
  3. Heading choice: the equivalent mod P for a target heading; the start
     equivalent that fits a sweep in a range; whether a stretch fits.
  4. Inputs per push: tangential flag, period n, optional range. Decoding
     them from `tools` is the job's.
* Out of scope: blade offset (3b), the sweep's wire fields (jobs).
* Overlap: `lib/planner/`.
* Checks: as branch 2. Human: a test cut with offset 0 tracking curves and
  corners; a cabled range never exceeded.
* Found while planning (after branch 2), to settle in its Planning:
  * Corners: at a `BREAK` the heading jumps, and XY rounds it by junction
    deviation today. A needs an A-only swivel block between the XY blocks
    (a change of axis set already stops). Lifting there is the job's.
  * Travel: A rotates once at the lift to the next contour's start heading
    (lines have κ = 0), rather than following travel.
  * The executor's `pointAt` (`executor.cpp:19`) is stateless; a Bézier can
    turn past 180°, so the heading is unwrapped tick to tick.
  * The curvature-jump limit needs a bound on A's instant speed change.
  * How tangential, period and range reach the Pico before the job decodes
    `tools`: a bring-up command, or the job branch first.

### Status

Not started.

## Branch 3b: `feature/planner-blade-offset`

### Plan

* Type: feature.
* Purpose: the knife axis follows `P + d·T + e·N`; XY limits at the axis
  point.
* Depends on: branch 3.
* Scope:
  1. The axis point from the tip path and heading; swivels about the tip at
     corners.
  2. XY speed and acceleration checked at the axis point.
  3. Input per push: `(d, e)`.
* Out of scope: reach (the job's preflight grows the bbox by r).
* Checks: as branch 2. Human: opposite-direction slits coincide.

### Status

Not started.

## Branch 4: `feature/planner-mesh`

### Plan

* Type: feature.
* Purpose: Z from the mesh under every XY move, with Z's speed and
  acceleration limits; `/mesh.bin` read at boot.
* Depends on: branch 2. Runs before branch 3.
* Scope:
  1. Mesh added between executor and followers at the selected tip, on
     every move once X, Y and Z are homed; flat planner Z (restart
     subtracts it); a lookup of the mesh Z at a point, for the job's lifts
     and plunges (none exist yet).
  2. The push-time walk: Z limits along XY blocks (a grid-line crossing is
     a junction: v² ≤ Z `maxAccel` · spacing / Δslope), the per-block
     soft-range check (a held XY jog ends where Z would leave it); the
     load-time worst-case report.
  3. `/mesh.bin` decode into RAM, flat when missing or bad; `mesh on|off`
     (volatile, on at boot per the config's required `machine.meshOn`,
     latched at the next restart);
     `get` key `mesh=<nx>x<ny>|off|flat|bad`; `WPos` Z without the mesh.
  4. A bench mesh flashed by `uploadfs` (`data/mesh.bin`), encoded by
     `web/scripts/mesh-image.ts` from `custom_mesh_json`
     (`config/mesh-bench.jsonc`). Maths in `lib/planner/planner/mesh.h`,
     load and latch in `core0/ops/mesh.{h,cpp}`.
  5. Docs: `docs/wire_protocol.md` (`get` keys), the mesh file.
* Out of scope: probing the mesh.
* Overlap: `web/src/wire/`.
* Checks: as branch 2. Human: a tilted test mesh followed on a line, a
  curve and a jog; the tick's worst-case cycles with the mesh on and off.

### Status

Done: merged to `main`. Unblocks the motion-sessions job branch (lift
insertions), then branch 3.

### Outcome

* Added in Work: `machine.meshOn`, required by the web loader and the Pico
  decoder (an older `config.bin` is `missing` until `uploadfs`); the mesh
  starts off without a config. `config/mesh-jagged.jsonc`, a 10 mm
  checkerboard on a 100 mm grid, for seeing Z follow.
* For branch 3 and motion-sessions: `Planner::pushMove`/`pushLine`/
  `pushBezier` take an optional `PathCap` (v² and accel ceilings); the
  queue's walk runs under `plannerLock`. A move refused for Z under the mesh
  is `PQ_SOFT_LIMIT` (`err soft_limit z`, `NACK_SOFT_LIMIT`).
* The load-time worst-case report is `get meshslope` (steepest neighbour
  slope); the per-block caps do the rest.
* The offset is relative to the work origin, so with Z at park any bed
  higher than the origin is out of range: XY jogs from park stop there.
  Lower Z first. A clearer reply or a park rule is open.
* Bench (head 0, node 8 excluded, `select 0` needed after `bus_exclude`):
  tilted and jagged meshes followed on step and held jogs, `mesh on|off`
  without a jump, `wzero z` off the origin, soft-range refusal and the held
  jog's clip. Not bench-tested: records and `bez` under the mesh, the speed
  caps binding, head 1, the tick's cycles with the mesh on.
* Found: `pio test -e native` runs on Windows now; the `memset` veneer
  warning predates this branch.

## Open

* Tangential on XY travel lines: A follows, or rotates once at the lift?
* Mesh: the grid's extent and spacing defaults; whether the upload goes
  through the config file transfer.
